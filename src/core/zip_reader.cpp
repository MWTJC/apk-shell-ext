#include "core/zip_reader.h"

#include <cstring>
#include <string>

#include "core/file_util.h"
#include "miniz.h"

namespace apksx {
namespace {

constexpr uint32_t kSigEocd = 0x06054b50u;
constexpr uint32_t kSigZip64Locator = 0x07064b50u;
constexpr uint32_t kSigZip64Eocd = 0x06064b50u;
constexpr uint32_t kSigCentralDir = 0x02014b50u;
constexpr uint32_t kSigLocalHeader = 0x04034b50u;

constexpr size_t kEocdSize = 22;
constexpr size_t kMaxCommentScan = 0xFFFF + kEocdSize;  // 注释最长 65535
constexpr size_t kMaxCentralDirSize = 32ull * 1024 * 1024;

}  // namespace

ZipReader::~ZipReader() { Close(); }

void ZipReader::Close() {
  source_.reset();
  entries_.clear();
  cd_digest_.clear();
  file_size_ = 0;
  cd_offset_ = 0;
  cd_size_ = 0;
}

bool ZipReader::ReadAt(uint64_t offset, void* buffer, size_t length) const {
  if (!source_) return false;
  return source_->ReadAt(offset, buffer, length);
}

bool ZipReader::Open(const std::wstring& path, Error* err) {
  auto source = OpenFileSource(path, err);
  if (!source) return false;
  return OpenSource(source, err);
}

bool ZipReader::OpenSource(std::shared_ptr<ByteSource> source, Error* err) {
  Close();
  if (!source) {
    if (err) err->set(L"数据源为空");
    return false;
  }
  source_ = source;
  file_size_ = source_->size();
  if (file_size_ < kEocdSize) {
    if (err) err->set(L"文件太小，不是有效 ZIP");
    Close();
    return false;
  }

  // --- 从文件尾向前找 EOCD（注释长度自证，避免误命中数据区里的签名）
  const size_t tail_len = static_cast<size_t>(
      (file_size_ < kMaxCommentScan) ? file_size_ : kMaxCommentScan);
  std::vector<uint8_t> tail(tail_len);
  if (!ReadAt(file_size_ - tail_len, tail.data(), tail_len)) {
    if (err) err->set(L"读取文件尾部失败");
    Close();
    return false;
  }

  size_t eocd_pos = SIZE_MAX;  // 相对 tail 的偏移
  for (size_t i = tail_len - kEocdSize + 1; i-- > 0;) {
    if (le32(tail.data() + i) != kSigEocd) continue;
    const uint16_t comment_len = le16(tail.data() + i + 20);
    if (i + kEocdSize + comment_len == tail_len) {
      eocd_pos = i;
      break;
    }
  }
  if (eocd_pos == SIZE_MAX) {
    if (err) err->set(L"未找到 ZIP 结束记录（不是有效 ZIP）");
    Close();
    return false;
  }

  const uint8_t* eocd = tail.data() + eocd_pos;
  uint64_t total_entries = le16(eocd + 10);
  uint64_t cd_size = le32(eocd + 12);
  uint64_t cd_offset = le32(eocd + 16);
  const uint64_t eocd_abs = file_size_ - tail_len + eocd_pos;

  // --- ZIP64：ZIP64 EOCD 定位器紧挨在 EOCD 之前
  if (total_entries == 0xFFFFu || cd_offset == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu) {
    if (eocd_abs < 20) {
      if (err) err->set(L"ZIP64 定位器缺失");
      Close();
      return false;
    }
    uint8_t loc[20] = {};
    if (!ReadAt(eocd_abs - 20, loc, sizeof(loc)) || le32(loc) != kSigZip64Locator) {
      if (err) err->set(L"ZIP64 定位器无效");
      Close();
      return false;
    }
    const uint64_t z64_off = le64(loc + 8);
    uint8_t rec[56] = {};
    if (z64_off > file_size_ || file_size_ - z64_off < sizeof(rec) ||
        !ReadAt(z64_off, rec, sizeof(rec)) || le32(rec) != kSigZip64Eocd) {
      if (err) err->set(L"ZIP64 结束记录无效");
      Close();
      return false;
    }
    total_entries = le64(rec + 32);
    cd_size = le64(rec + 40);
    cd_offset = le64(rec + 48);
  }

  if (total_entries == 0) {
    if (err) err->set(L"ZIP 中央目录为空");
    Close();
    return false;
  }
  if (total_entries > kMaxZipEntries) {
    if (err) err->set(L"ZIP 条目数超限（>65535）");
    Close();
    return false;
  }
  if (cd_size > kMaxCentralDirSize || cd_offset > file_size_ ||
      cd_size > file_size_ - cd_offset) {
    if (err) err->set(L"ZIP 中央目录越界");
    Close();
    return false;
  }

  if (!ParseCentralDirectory(cd_offset, cd_size, static_cast<uint32_t>(total_entries), err)) {
    Close();
    return false;
  }
  // 签名块解析要用：中央目录的绝对位置在文件里是稳定的（哪怕是 ZIP64）
  cd_offset_ = cd_offset;
  cd_size_ = cd_size;
  return true;
}

bool ZipReader::ParseCentralDirectory(uint64_t cd_offset, uint64_t cd_size,
                                      uint32_t expected_entries, Error* err) {
  std::vector<uint8_t> cd(static_cast<size_t>(cd_size));
  if (!ReadAt(cd_offset, cd.data(), cd.size())) {
    if (err) err->set(L"读取中央目录失败");
    return false;
  }

  // 中央目录摘要：stream 场景下用来区分"同名不同内容"的文件（缓存键的一部分）
  {
    std::wstring hex;
    std::string utf8;
    if (Sha1Hex(cd.data(), cd.size(), &hex) && WideToUtf8(hex, &utf8)) {
      cd_digest_ = utf8;
    }
  }

  entries_.reserve(expected_entries);
  size_t pos = 0;
  while (pos + 46 <= cd.size()) {
    const uint8_t* p = cd.data() + pos;
    if (le32(p) != kSigCentralDir) break;

    const uint16_t name_len = le16(p + 28);
    const uint16_t extra_len = le16(p + 30);
    const uint16_t comment_len = le16(p + 32);
    const size_t need = 46u + name_len + extra_len + comment_len;
    if (pos + need > cd.size()) break;

    ZipEntry e;
    e.flags = le16(p + 8);
    e.method = le16(p + 10);
    e.crc32 = le32(p + 16);
    e.compressed_size = le32(p + 20);
    e.uncompressed_size = le32(p + 24);
    e.local_header_offset = le32(p + 42);
    e.name.assign(reinterpret_cast<const char*>(p + 46), name_len);

    // ZIP64 扩展字段：只覆盖被置为 0xFFFFFFFF 的字段，顺序固定
    const uint8_t* extra = p + 46 + name_len;
    size_t extra_pos = 0;
    while (extra_pos + 4 <= extra_len) {
      const uint16_t id = le16(extra + extra_pos);
      const uint16_t len = le16(extra + extra_pos + 2);
      if (extra_pos + 4 + len > extra_len) break;
      if (id == 0x0001) {
        const uint8_t* q = extra + extra_pos + 4;
        size_t qpos = 0;
        auto take64 = [&](uint64_t* field) {
          if (qpos + 8 <= len) {
            *field = le64(q + qpos);
            qpos += 8;
          }
        };
        if (e.uncompressed_size == 0xFFFFFFFFu) take64(&e.uncompressed_size);
        if (e.compressed_size == 0xFFFFFFFFu) take64(&e.compressed_size);
        if (e.local_header_offset == 0xFFFFFFFFu) take64(&e.local_header_offset);
      }
      extra_pos += 4u + len;
    }

    if (e.local_header_offset >= file_size_ || e.compressed_size > file_size_ ||
        e.local_header_offset + e.compressed_size > file_size_) {
      // 条目越界：跳过而不是整体失败（现实里有半坏包，但可用部分仍应能取图标）
      pos += need;
      continue;
    }

    entries_.push_back(std::move(e));
    pos += need;
    if (entries_.size() >= kMaxZipEntries) break;
  }

  if (entries_.empty()) {
    if (err) err->set(L"中央目录里没有可用条目");
    return false;
  }
  return true;
}

const ZipEntry* ZipReader::Find(std::string_view name) const {
  for (const ZipEntry& e : entries_) {
    if (e.name.size() == name.size() &&
        std::memcmp(e.name.data(), name.data(), name.size()) == 0) {
      return &e;
    }
  }
  return nullptr;
}

bool ZipReader::Read(const ZipEntry& entry, uint64_t limit, std::vector<uint8_t>* out,
                     Error* err) const {
  if (out == nullptr) return false;
  out->clear();

  if ((entry.flags & 0x0001u) != 0) {  // 加密条目：不弹密码框，直接拒绝
    if (err) err->set(L"条目已加密");
    return false;
  }
  if (entry.uncompressed_size > limit) {
    if (err) err->set(L"条目解压后超过上限");
    return false;
  }

  // 定位数据：本地头长度不可信，按中央目录给出的偏移重算
  uint8_t local[30] = {};
  if (!ReadAt(entry.local_header_offset, local, sizeof(local)) || le32(local) != kSigLocalHeader) {
    if (err) err->set(L"本地文件头无效");
    return false;
  }
  const uint64_t data_offset = entry.local_header_offset + 30u + le16(local + 26) +
                               le16(local + 28);
  if (data_offset > file_size_ || entry.compressed_size > file_size_ - data_offset) {
    if (err) err->set(L"条目数据越界");
    return false;
  }

  std::vector<uint8_t> compressed(static_cast<size_t>(entry.compressed_size));
  if (!compressed.empty() && !ReadAt(data_offset, compressed.data(), compressed.size())) {
    if (err) err->set(L"读取条目数据失败");
    return false;
  }

  if (entry.method == 0) {  // store
    if (compressed.size() > limit) {
      if (err) err->set(L"条目解压后超过上限");
      return false;
    }
    *out = std::move(compressed);
    return true;
  }

  if (entry.method != 8) {  // 只支持 deflate（APK 事实上只用 0/8）
    if (err) err->set(L"不支持的压缩方法");
    return false;
  }

  // 解压上限按 limit 分配：声明大小造假时 tinfl 会因目标缓冲不足而失败，不会炸内存。
  const size_t cap = static_cast<size_t>(
      (entry.uncompressed_size > 0 && entry.uncompressed_size <= limit) ? entry.uncompressed_size
                                                                      : limit);
  out->resize(cap);
  const size_t produced = tinfl_decompress_mem_to_mem(out->data(), out->size(),
                                                      compressed.data(), compressed.size(), 0);
  if (produced == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
    out->clear();
    if (err) err->set(L"deflate 解压失败（数据损坏或超限）");
    return false;
  }
  out->resize(produced);
  return true;
}

std::shared_ptr<ByteSource> ZipReader::OpenStoredEntry(const ZipEntry& entry) const {
  if (!source_ || entry.method != 0) return nullptr;
  if ((entry.flags & 0x0001u) != 0) return nullptr;  // 加密条目不解

  uint8_t local[30] = {};
  if (!ReadAt(entry.local_header_offset, local, sizeof(local)) || le32(local) != kSigLocalHeader) {
    return nullptr;
  }
  const uint64_t data_offset =
      entry.local_header_offset + 30u + le16(local + 26) + le16(local + 28);
  if (data_offset > source_->size() || entry.uncompressed_size > source_->size() - data_offset) {
    return nullptr;
  }
  return SliceSource(source_, data_offset, entry.uncompressed_size);
}

}  // namespace apksx

#include "core/arsc_stream.h"

#include <cstring>

#include "core/axml.h"

namespace apksx {
namespace {

constexpr uint16_t kResTableType = 0x0002;
constexpr uint16_t kResStringPoolType = 0x0001;
constexpr uint16_t kResTablePackageType = 0x0200;
constexpr uint16_t kResTableTypeType = 0x0201;
constexpr uint8_t kTypeFlagSparse = 0x01;
constexpr uint8_t kTypeFlagOffset16 = 0x02;
constexpr uint16_t kEntryFlagComplex = 0x0001;

constexpr size_t kConfigDensityOffset = 14;
constexpr size_t kConfigSdkVersionOffset = 24;
constexpr size_t kConfigLanguageOffset = 8;
constexpr size_t kConfigCountryOffset = 10;

constexpr uint32_t kStringPoolUtf8Flag = 0x100;
constexpr size_t kMaxChunkHeaderRead = 256;   // type chunk 头 + 配置，一次读够
constexpr size_t kMaxSparseScan = 1u << 20;   // 稀疏表的偏移数组最多扫 1 MB
constexpr uint32_t kMaxStringBytes = 1u << 20;

std::string UnpackLocaleChars(const uint8_t* p) {
  std::string out;
  for (int i = 0; i < 2; ++i) {
    const uint8_t c = p[i];
    if (c == 0) break;
    if (c < 0x20 || c > 0x7E) return std::string();
    out.push_back(static_cast<char>(c));
  }
  return out;
}

}  // namespace

bool ArscStreamTable::ReadAt(uint64_t offset, void* buffer, size_t length) const {
  if (!src_ || offset > size_ || length > size_ - offset) return false;
  return src_->ReadAt(offset, buffer, length);
}

bool ArscStreamTable::Open(std::shared_ptr<ByteSource> arsc, Error* err) {
  src_ = std::move(arsc);
  packages_.clear();
  global_pool_offset_ = 0;
  global_pool_size_ = 0;
  size_ = 0;

  if (!src_) {
    if (err) err->set(L"arsc 数据源为空");
    return false;
  }
  size_ = src_->size();
  if (size_ < 12) {
    if (err) err->set(L"resources.arsc 太小");
    return false;
  }

  uint8_t head[12] = {};
  if (!ReadAt(0, head, sizeof(head)) || le16(head) != kResTableType) {
    if (err) err->set(L"不是 RES_TABLE");
    return false;
  }
  const uint16_t table_header = le16(head + 2);
  const uint32_t table_size = le32(head + 4);
  const uint64_t table_end = (table_size >= table_header && table_size <= size_) ? table_size : size_;

  uint64_t pos = table_header;
  while (pos + 8 <= table_end) {
    uint8_t chunk[8] = {};
    if (!ReadAt(pos, chunk, sizeof(chunk))) break;
    const ChunkHeader ch = ReadChunkHeader(chunk);
    if (ch.size < 8 || pos + ch.size > table_end) break;

    if (ch.type == kResStringPoolType && global_pool_offset_ == 0) {
      global_pool_offset_ = pos;
      global_pool_size_ = ch.size;
    } else if (ch.type == kResTablePackageType && ch.header_size >= 284) {
      // 包头的 typeIdOffset 在偏移 284，所以这里必须读到 288 —— 只读 256 会读到越界垃圾，
      // 把它当成 typeIdOffset 后所有 type 都对不上号（实测在混淆包上"静默丢图"）。
      uint8_t pkg[320] = {};
      const size_t want = (ch.header_size < sizeof(pkg)) ? ch.header_size : sizeof(pkg);
      if (ReadAt(pos, pkg, want)) {
        Package p;
        p.id = pkg[8];
        p.header_size = ch.header_size;
        p.type_id_offset = (want >= 285) ? pkg[284] : static_cast<uint8_t>(0);
        p.offset = pos;
        p.size = ch.size;
        packages_.push_back(p);
      }
    }
    pos += ch.size;
  }
  return true;
}

bool ArscStreamTable::ReadStringAt(uint64_t pool_offset, uint64_t pool_size, uint32_t index,
                                   std::string* out) const {
  if (out == nullptr || pool_size < 28) return false;
  uint8_t head[28] = {};
  if (!ReadAt(pool_offset, head, sizeof(head))) return false;
  if (le16(head) != kResStringPoolType) return false;

  const uint32_t string_count = le32(head + 8);
  const uint32_t flags = le32(head + 16);
  const uint32_t strings_start = le32(head + 20);
  const uint16_t header_size = le16(head + 2);
  if (index >= string_count) return false;

  const uint64_t offset_entry = pool_offset + header_size + static_cast<uint64_t>(index) * 4u;
  uint8_t raw_off[4] = {};
  if (!ReadAt(offset_entry, raw_off, sizeof(raw_off))) return false;
  const uint32_t rel = le32(raw_off);
  if (rel == 0xFFFFFFFFu || strings_start >= pool_size) return false;

  const uint64_t string_base = pool_offset + strings_start + rel;
  if (string_base >= pool_offset + pool_size) return false;

  const bool utf8 = (flags & kStringPoolUtf8Flag) != 0;
  uint8_t len_bytes[4] = {};
  if (!ReadAt(string_base, len_bytes, sizeof(len_bytes))) return false;

  out->clear();
  if (utf8) {
    size_t pos = 0;
    if (len_bytes[pos] & 0x80) pos += 2; else pos += 1;  // UTF-16 字符数（跳过）
    uint32_t u8len = len_bytes[pos];
    if ((u8len & 0x80u) != 0) {
      u8len = ((u8len & 0x7Fu) << 8) | len_bytes[pos + 1];
      pos += 2;
    } else {
      pos += 1;
    }
    if (u8len == 0 || u8len > kMaxStringBytes) return true;  // 空串是合法的
    out->resize(u8len);
    return ReadAt(string_base + pos, out->data(), u8len);
  }

  uint32_t len = le16(len_bytes);
  size_t pos = 2;
  if ((len & 0x8000u) != 0) {
    len = ((len & 0x7FFFu) << 16) | le16(len_bytes + 2);
    pos = 4;
  }
  if (len == 0 || len > kMaxStringBytes / 2) return true;
  std::vector<uint8_t> utf16(static_cast<size_t>(len) * 2u);
  if (!ReadAt(string_base + pos, utf16.data(), utf16.size())) return false;
  std::wstring wide(static_cast<size_t>(len), L'\0');
  for (uint32_t i = 0; i < len; ++i) wide[i] = static_cast<wchar_t>(le16(utf16.data() + i * 2u));
  return WideToUtf8(wide, out);
}

bool ArscStreamTable::GetGlobalString(uint32_t index, std::string* out) const {
  if (global_pool_offset_ == 0) return false;
  return ReadStringAt(global_pool_offset_, global_pool_size_, index, out);
}

void ArscStreamTable::ScanTypeChunk(uint64_t chunk_offset, uint16_t header_size,
                                    uint32_t chunk_size, uint8_t type_id_offset, uint8_t want_type,
                                    uint32_t want_entry,
                                    std::vector<ArscCandidate>* out) const {
  if (out == nullptr || header_size < 20 || chunk_size < header_size) return;

  uint8_t buf[kMaxChunkHeaderRead] = {};
  const size_t want = (header_size < kMaxChunkHeaderRead) ? header_size : kMaxChunkHeaderRead;
  if (!ReadAt(chunk_offset, buf, want)) return;

  const uint8_t raw_type_id = buf[8];
  const uint8_t flags = buf[9];
  const uint32_t entry_count = le32(buf + 12);
  const uint32_t entries_start = le32(buf + 16);
  const uint8_t effective_type_id = static_cast<uint8_t>(raw_type_id + type_id_offset);
  if (effective_type_id != want_type) return;
  if (want_entry >= entry_count) return;

  uint16_t density = kDensityDefault;
  uint16_t sdk = 0;
  std::string language;
  std::string country;
  if (want >= 20 + 4) {
    const uint32_t cfg_size = le32(buf + 20);
    if (cfg_size >= kConfigDensityOffset + 2 && want >= 20 + kConfigDensityOffset + 2) {
      density = le16(buf + 20 + kConfigDensityOffset);
    }
    if (cfg_size >= kConfigSdkVersionOffset + 2 && want >= 20 + kConfigSdkVersionOffset + 2) {
      sdk = le16(buf + 20 + kConfigSdkVersionOffset);
    }
    if (cfg_size >= kConfigCountryOffset + 2 && want >= 20 + kConfigCountryOffset + 2) {
      language = UnpackLocaleChars(buf + 20 + kConfigLanguageOffset);
      country = UnpackLocaleChars(buf + 20 + kConfigCountryOffset);
    }
  }

  const bool sparse = (flags & kTypeFlagSparse) != 0;
  const bool offset16 = (flags & kTypeFlagOffset16) != 0;
  const uint64_t offsets_base = chunk_offset + header_size;
  const uint64_t chunk_end = chunk_offset + chunk_size;

  uint64_t rel = 0;
  bool found = false;

  if (!sparse && !offset16) {
    uint8_t raw[4] = {};
    if (offsets_base + static_cast<uint64_t>(want_entry) * 4u + 4u > chunk_end) return;
    if (!ReadAt(offsets_base + static_cast<uint64_t>(want_entry) * 4u, raw, sizeof(raw))) return;
    rel = le32(raw);
    found = (rel != 0xFFFFFFFFu);
  } else if (offset16) {
    uint8_t raw[2] = {};
    if (offsets_base + static_cast<uint64_t>(want_entry) * 2u + 2u > chunk_end) return;
    if (!ReadAt(offsets_base + static_cast<uint64_t>(want_entry) * 2u, raw, sizeof(raw))) return;
    const uint16_t v = le16(raw);
    found = (v != 0xFFFFu);
    rel = static_cast<uint64_t>(v) * 4u;
  } else {
    // 稀疏表：成对的 (uint16 idx, uint16 offset/4)。分块扫描，避免一次读整张表。
    const uint64_t total = static_cast<uint64_t>(entry_count) * 4u;
    const uint64_t limit = (total < kMaxSparseScan) ? total : kMaxSparseScan;
    std::vector<uint8_t> pairs(4096);
    uint64_t scanned = 0;
    while (scanned < limit && !found) {
      const size_t n = static_cast<size_t>(
          (limit - scanned) < pairs.size() ? (limit - scanned) : pairs.size());
      if (offsets_base + scanned + n > chunk_end) break;
      if (!ReadAt(offsets_base + scanned, pairs.data(), n)) break;
      for (size_t i = 0; i + 4 <= n; i += 4) {
        if (le16(pairs.data() + i) == want_entry) {
          rel = static_cast<uint64_t>(le16(pairs.data() + i + 2)) * 4u;
          found = true;
          break;
        }
      }
      scanned += n;
    }
  }
  if (!found) return;

  const uint64_t entry_abs = chunk_offset + entries_start + rel;
  uint8_t entry[8] = {};
  if (entry_abs + sizeof(entry) > chunk_end) return;
  if (!ReadAt(entry_abs, entry, sizeof(entry))) return;
  const uint16_t entry_size = le16(entry);
  const uint16_t entry_flags = le16(entry + 2);
  if (entry_size < 8) return;

  ArscCandidate cand;
  cand.type_id = effective_type_id;
  cand.entry_index = want_entry;
  cand.density = density;
  cand.sdk_version = sdk;
  cand.language = language;
  cand.country = country;

  if ((entry_flags & kEntryFlagComplex) != 0) {
    cand.is_bag = true;
    out->push_back(std::move(cand));
    return;
  }

  uint8_t value[8] = {};
  if (entry_abs + entry_size + sizeof(value) > chunk_end) return;
  if (!ReadAt(entry_abs + entry_size, value, sizeof(value))) return;
  if (le16(value) < 8) return;
  cand.value_type = value[3];
  cand.value_data = le32(value + 4);
  if (cand.value_type == kResTypeString) {
    GetGlobalString(cand.value_data, &cand.value_string);
  }
  out->push_back(std::move(cand));
}

void ArscStreamTable::CollectCandidates(uint32_t res_id,
                                        std::vector<ArscCandidate>* out) const {
  if (out == nullptr || res_id == kInvalidResId) return;
  const uint8_t want_pkg = static_cast<uint8_t>(res_id >> 24);
  const uint8_t want_type = static_cast<uint8_t>((res_id >> 16) & 0xFF);
  const uint32_t want_entry = res_id & 0xFFFFu;

  for (const Package& pkg : packages_) {
    if (pkg.id != want_pkg) continue;
    uint64_t pos = pkg.offset + pkg.header_size;
    const uint64_t end = pkg.offset + pkg.size;
    while (pos + 8 <= end) {
      uint8_t chunk[8] = {};
      if (!ReadAt(pos, chunk, sizeof(chunk))) break;
      const ChunkHeader ch = ReadChunkHeader(chunk);
      if (ch.size < 8 || pos + ch.size > end) break;
      if (ch.type == kResTableTypeType) {
        ScanTypeChunk(pos, ch.header_size, ch.size, pkg.type_id_offset, want_type, want_entry, out);
      }
      pos += ch.size;
    }
  }
}

void ArscStreamTable::CollectConfigs(std::vector<ArscConfig>* out) const {
  if (out == nullptr) return;
  for (const Package& pkg : packages_) {
    uint64_t pos = pkg.offset + pkg.header_size;
    const uint64_t end = pkg.offset + pkg.size;
    while (pos + 8 <= end) {
      uint8_t chunk[8] = {};
      if (!ReadAt(pos, chunk, sizeof(chunk))) break;
      const ChunkHeader ch = ReadChunkHeader(chunk);
      if (ch.size < 8 || pos + ch.size > end) break;
      if (ch.type == kResTableTypeType && ch.header_size >= 20 + 4) {
        uint8_t buf[kMaxChunkHeaderRead] = {};
        const size_t want =
            (ch.header_size < kMaxChunkHeaderRead) ? ch.header_size : kMaxChunkHeaderRead;
        if (ReadAt(pos, buf, want) && want >= 20 + kConfigCountryOffset + 2) {
          const uint32_t cfg_size = le32(buf + 20);
          ArscConfig cfg;
          if (cfg_size >= kConfigDensityOffset + 2) {
            cfg.density = le16(buf + 20 + kConfigDensityOffset);
          }
          if (cfg_size >= kConfigCountryOffset + 2) {
            cfg.language = UnpackLocaleChars(buf + 20 + kConfigLanguageOffset);
            cfg.country = UnpackLocaleChars(buf + 20 + kConfigCountryOffset);
          }
          if (cfg_size >= kConfigSdkVersionOffset + 2 && want >= 20 + kConfigSdkVersionOffset + 2) {
            cfg.sdk_version = le16(buf + 20 + kConfigSdkVersionOffset);
          }
          out->push_back(std::move(cfg));
        }
      }
      pos += ch.size;
    }
  }
}

}  // namespace apksx

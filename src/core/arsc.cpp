#include "core/arsc.h"

#include "core/axml.h"

#include <cstring>

namespace apksx {
namespace {

constexpr uint16_t kResTableType = 0x0002;
constexpr uint16_t kResTablePackageType = 0x0200;
constexpr uint16_t kResTableTypeType = 0x0201;
constexpr uint16_t kResTableTypeSpecType = 0x0202;

// ResTable_type::flags
constexpr uint8_t kTypeFlagSparse = 0x01;
constexpr uint8_t kTypeFlagOffset16 = 0x02;

constexpr uint16_t kEntryFlagComplex = 0x0001;

// ResTable_config 内 density 的字节偏移（size 4 + mcc/mnc 4 + language/country 4 +
// orientation 1 + touchscreen 1）
constexpr size_t kConfigDensityOffset = 14;
constexpr size_t kConfigSdkVersionOffset = 24;

class ArscWalker {
 public:
  ArscWalker(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool Walk(uint32_t res_id, std::vector<ArscCandidate>* out, StringPool* global_pool,
            std::vector<ArscPackageInfo>* packages, Error* err);

 private:
  void WalkPackage(const uint8_t* pkg, size_t pkg_size, uint8_t want_pkg, uint8_t want_type,
                   uint32_t want_entry, StringPool* global_pool, std::vector<ArscCandidate>* out);
  void WalkType(const uint8_t* chunk, size_t chunk_size, uint8_t pkg_type_id_offset,
                uint8_t want_type, uint32_t want_entry, StringPool* global_pool,
                std::vector<ArscCandidate>* out);

  const uint8_t* data_;
  size_t size_;
};

void ArscWalker::WalkType(const uint8_t* chunk, size_t chunk_size, uint8_t type_id_offset,
                          uint8_t want_type, uint32_t want_entry, StringPool* global_pool,
                          std::vector<ArscCandidate>* out) {
  const ChunkHeader h = ReadChunkHeader(chunk);
  if (h.type != kResTableTypeType || h.size > chunk_size || h.header_size < 20) return;
  const size_t chunk_end = h.size;

  const uint8_t raw_type_id = chunk[8];
  const uint8_t flags = chunk[9];
  const uint32_t entry_count = le32(chunk + 12);
  const uint32_t entries_start = le32(chunk + 16);
  const uint8_t effective_type_id = static_cast<uint8_t>(raw_type_id + type_id_offset);
  if (effective_type_id != want_type) return;
  if (want_entry >= entry_count) return;

  uint16_t density = kDensityDefault;
  uint16_t sdk = 0;
  if (h.header_size >= 20 + 4) {
    const uint32_t cfg_size = le32(chunk + 20);
    if (cfg_size >= kConfigDensityOffset + 2 && h.header_size >= 20 + kConfigDensityOffset + 2) {
      density = le16(chunk + 20 + kConfigDensityOffset);
    }
    if (cfg_size >= kConfigSdkVersionOffset + 2 &&
        h.header_size >= 20 + kConfigSdkVersionOffset + 2) {
      sdk = le16(chunk + 20 + kConfigSdkVersionOffset);
    }
  }

  const bool sparse = (flags & kTypeFlagSparse) != 0;
  const bool offset16 = (flags & kTypeFlagOffset16) != 0;
  const uint8_t* offsets = chunk + h.header_size;
  const size_t per_entry = offset16 ? 2u : 4u;
  const size_t offsets_bytes = static_cast<size_t>(entry_count) * per_entry;
  if (h.header_size + offsets_bytes > chunk_end) return;

  uint64_t rel = 0;
  bool found = false;

  if (!sparse && !offset16) {
    rel = le32(offsets + static_cast<size_t>(want_entry) * 4u);
    found = (rel != 0xFFFFFFFFu);
  } else if (offset16) {
    const uint16_t v = le16(offsets + static_cast<size_t>(want_entry) * 2u);
    found = (v != 0xFFFFu);
    rel = static_cast<uint64_t>(v) * 4u;
  } else {
    // 稀疏：entry_count 为 pairs 数量，每对 (uint16 index, uint16 offset/4)
    for (uint32_t i = 0; i < entry_count; ++i) {
      const uint16_t idx = le16(offsets + static_cast<size_t>(i) * 4u);
      const uint16_t off = le16(offsets + static_cast<size_t>(i) * 4u + 2);
      if (idx == want_entry) {
        rel = static_cast<uint64_t>(off) * 4u;
        found = true;
        break;
      }
    }
  }
  if (!found) return;

  const uint64_t entry_abs = static_cast<uint64_t>(entries_start) + rel;
  if (entry_abs + 8 > chunk_end) return;

  const uint8_t* entry = chunk + entry_abs;
  const uint16_t entry_size = le16(entry);
  const uint16_t entry_flags = le16(entry + 2);
  if (entry_size < 8 || entry_abs + entry_size > chunk_end) return;

  ArscCandidate cand;
  cand.type_id = effective_type_id;
  cand.entry_index = want_entry;
  cand.density = density;
  cand.sdk_version = sdk;

  if ((entry_flags & kEntryFlagComplex) != 0) {
    cand.is_bag = true;
    out->push_back(std::move(cand));
    return;
  }

  // Res_value 紧跟在 ResTable_entry 之后
  const uint64_t value_abs = entry_abs + entry_size;
  if (value_abs + 8 > chunk_end) return;
  const uint8_t* value = chunk + value_abs;
  const uint8_t value_size = static_cast<uint8_t>(le16(value));
  if (value_size < 8) return;
  cand.value_type = value[3];
  cand.value_data = le32(value + 4);
  if (cand.value_type == kResTypeString && global_pool != nullptr) {
    global_pool->Get(cand.value_data, &cand.value_string);
  }
  out->push_back(std::move(cand));
}

void ArscWalker::WalkPackage(const uint8_t* pkg, size_t pkg_size, uint8_t want_pkg,
                             uint8_t want_type, uint32_t want_entry, StringPool* global_pool,
                             std::vector<ArscCandidate>* out) {
  const ChunkHeader h = ReadChunkHeader(pkg);
  if (h.type != kResTablePackageType || h.size > pkg_size || h.header_size < 284) return;

  const uint8_t pkg_id = pkg[8];
  if (pkg_id != want_pkg) return;

  const uint8_t type_id_offset =
      (h.header_size >= 288) ? pkg[284] : static_cast<uint8_t>(0);

  size_t pos = h.header_size;
  while (pos + 8 <= h.size) {
    const uint8_t* sub = pkg + pos;
    const ChunkHeader sh = ReadChunkHeader(sub);
    if (sh.size < 8 || pos + sh.size > h.size) break;
    if (sh.type == kResTableTypeType) {
      WalkType(sub, sh.size, type_id_offset, want_type, want_entry, global_pool, out);
    }
    pos += sh.size;
  }
}

bool ArscWalker::Walk(uint32_t res_id, std::vector<ArscCandidate>* out, StringPool* global_pool,
                      std::vector<ArscPackageInfo>* packages, Error* err) {
  if (size_ < 12) {
    if (err) err->set(L"resources.arsc 太小");
    return false;
  }
  const ChunkHeader table = ReadChunkHeader(data_);
  if (table.type != kResTableType) {
    if (err) err->set(L"不是 RES_TABLE");
    return false;
  }
  const size_t table_end = (table.size <= size_) ? table.size : size_;

  const uint8_t want_pkg = static_cast<uint8_t>(res_id >> 24);
  const uint8_t want_type = static_cast<uint8_t>((res_id >> 16) & 0xFF);
  const uint32_t want_entry = res_id & 0xFFFFu;

  size_t pos = table.header_size;
  while (pos + 8 <= table_end) {
    const uint8_t* p = data_ + pos;
    const ChunkHeader ch = ReadChunkHeader(p);
    if (ch.size < 8 || pos + ch.size > table_end) break;

    if (ch.type == 0x0001) {  // 全局字符串池
      if (global_pool != nullptr) global_pool->Parse(p, ch.size, nullptr);
    } else if (ch.type == kResTablePackageType) {
      if (packages != nullptr && ch.header_size >= 268) {
        ArscPackageInfo info;
        info.id = p[8];
        std::wstring wide;
        for (size_t i = 0; i < 128; ++i) {
          const uint16_t c = le16(p + 12 + i * 2);
          if (c == 0) break;
          wide.push_back(static_cast<wchar_t>(c));
        }
        WideToUtf8(wide, &info.name);
        packages->push_back(std::move(info));
      }
      WalkPackage(p, ch.size, want_pkg, want_type, want_entry, global_pool, out);
    }
    pos += ch.size;
  }
  return true;
}

}  // namespace

bool ArscTable::Parse(const std::vector<uint8_t>& data, Error* err) {
  data_ = data;
  packages_.clear();
  parsed_ = false;

  ArscWalker walker(data_.data(), data_.size());
  // 只做一次"发现包 + 建全局字符串池"的遍历（res_id 用不到的类型会被跳过）
  std::vector<ArscCandidate> scratch;
  if (!walker.Walk(kInvalidResId, &scratch, &global_strings_, &packages_, err)) return false;
  parsed_ = true;
  return true;
}

void ArscTable::CollectCandidates(uint32_t res_id, std::vector<ArscCandidate>* out) const {
  if (out == nullptr) return;
  out->clear();
  if (!parsed_ || res_id == kInvalidResId || data_.empty()) return;

  ArscWalker walker(data_.data(), data_.size());
  StringPool pool = global_strings_;
  walker.Walk(res_id, out, &pool, nullptr, nullptr);
}

bool ArscTable::GetGlobalString(uint32_t index, std::string* out) const {
  return global_strings_.Get(index, out);
}

bool ArscTable::HasPackage(uint8_t id) const {
  for (const ArscPackageInfo& p : packages_) {
    if (p.id == id) return true;
  }
  return false;
}

}  // namespace apksx

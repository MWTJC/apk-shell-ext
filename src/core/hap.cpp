#include "core/hap.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "core/json_lite.h"
#include "core/zip_reader.h"

namespace apksx {
namespace {

// module.json 只有一两 KB，给足余量
constexpr uint64_t kMaxHapManifest = 4ull * 1024 * 1024;
constexpr uint64_t kMaxLayeredJson = 64ull * 1024;
constexpr uint64_t kMaxHapIconRead = 16ull * 1024 * 1024;
constexpr uint64_t kMaxHapIndexRead = 64ull * 1024 * 1024;

std::string LowerAscii(const std::string& in) {
  std::string out = in;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool EndsWithNoCase(const std::string& name, const char* suffix) {
  const size_t len = std::char_traits<char>::length(suffix);
  if (name.size() < len) return false;
  return LowerAscii(name.substr(name.size() - len)) == LowerAscii(suffix);
}

const json::Value* Field(const json::Value* obj, const char* key) {
  return obj == nullptr ? nullptr : obj->Find(key);
}

std::string StrField(const json::Value* obj, const char* key) {
  const json::Value* value = Field(obj, key);
  if (value == nullptr || !value->is_string()) return std::string();
  return std::string(value->as_string());
}

uint32_t UintField(const json::Value* obj, const char* key) {
  const json::Value* value = Field(obj, key);
  if (value == nullptr) return 0;
  const double number = value->as_number(0.0);
  return number <= 0.0 ? 0u : static_cast<uint32_t>(number);
}

std::vector<std::string> StrArrayField(const json::Value* obj, const char* key) {
  std::vector<std::string> out;
  const json::Value* value = Field(obj, key);
  if (value == nullptr || value->type != json::Type::Array) return out;
  for (const json::ValuePtr& item : value->items) {
    if (item != nullptr && item->is_string()) out.push_back(std::string(item->as_string()));
  }
  return out;
}

size_t ArraySize(const json::Value* obj, const char* key) {
  const json::Value* value = Field(obj, key);
  if (value == nullptr || value->type != json::Type::Array) return 0;
  return value->items.size();
}

bool ReadEntryText(const ZipReader& zip, const std::string& name, uint64_t limit,
                   std::string* out) {
  const ZipEntry* entry = zip.Find(name);
  if (entry == nullptr) return false;
  std::vector<uint8_t> bytes;
  if (!zip.Read(*entry, limit, &bytes, nullptr)) return false;
  out->assign(bytes.begin(), bytes.end());
  return true;
}

bool IsNumeric(const std::string& value) {
  if (value.empty()) return false;
  for (char c : value) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

// "$media:xxx" → "xxx"。其它前缀（$string:/$profile:/字面路径）一律返回 false。
bool MediaRefName(const std::string& ref, std::string* out) {
  const char* prefix = "$media:";
  const size_t len = std::char_traits<char>::length(prefix);
  if (ref.size() <= len || ref.compare(0, len, prefix) != 0) return false;
  *out = ref.substr(len);
  return !out->empty();
}

const char* const kIconExtensions[] = {".png", ".webp", ".jpg", ".jpeg"};

// 名字 → HAP 内路径：先 resources/base/media/<名>.<ext>，再任意 .../media/<名>.<ext>（优先路径里带 base 的）
bool FindMediaFile(const ZipReader& zip, const std::string& name, std::string* path) {
  for (const char* ext : kIconExtensions) {
    const std::string candidate = "resources/base/media/" + name + ext;
    if (zip.Find(candidate) != nullptr) {
      *path = candidate;
      return true;
    }
  }
  const std::string needle = "/media/" + LowerAscii(name);
  std::vector<std::string> matches;
  for (const ZipEntry& entry : zip.entries()) {
    const std::string lower = LowerAscii(entry.name);
    if (lower.find(needle) == std::string::npos) continue;
    bool good = false;
    for (const char* ext : kIconExtensions) {
      if (EndsWithNoCase(lower, ext)) {
        good = true;
        break;
      }
    }
    if (good) matches.push_back(entry.name);
  }
  if (matches.empty()) return false;
  std::sort(matches.begin(), matches.end(), [](const std::string& a, const std::string& b) {
    const bool a_base = LowerAscii(a).find("resources/base/") != std::string::npos;
    const bool b_base = LowerAscii(b).find("resources/base/") != std::string::npos;
    if (a_base != b_base) return a_base;
    return a < b;
  });
  *path = matches.front();
  return true;
}

// layered icon：{"layered-image":{"background":"$media:16777217","foreground":"$media:16777218"}}
// 鸿蒙这里引用的是**资源 id**，拿不到 id→文件名表（要解 resources.index），
// 所以按 JSON 的键名约定推断文件名（foreground.png / background.png），先前景后背景。
bool ResolveLayered(const ZipReader& zip, const std::string& json_path, std::string* path,
                    std::string* note) {
  std::string text;
  if (!ReadEntryText(zip, json_path, kMaxLayeredJson, &text)) return false;
  json::ValuePtr doc;
  Error err;
  if (!json::Parse(text, &doc, &err) || doc == nullptr) return false;
  const json::Value* layered = Field(doc.get(), "layered-image");
  if (layered == nullptr) return false;

  for (const char* layer : {"foreground", "background"}) {
    const std::string ref = StrField(layered, layer);
    if (ref.empty()) continue;
    std::string media;
    if (!MediaRefName(ref, &media)) continue;
    // 引用是资源 id（纯数字）时，用键名当文件名；否则两种都试
    if (!IsNumeric(media) && FindMediaFile(zip, media, path)) {
      *note = std::string("layered icon（") + layer + " 层，" + media + "）";
      return true;
    }
    if (FindMediaFile(zip, layer, path)) {
      *note = std::string("layered icon（") + layer + " 层，按键名推断）";
      return true;
    }
  }
  return false;
}

// "$media:X" / 字面路径 → HAP 内文件；layered_image 的 json 也在这里展开
bool ResolveMediaRef(const ZipReader& zip, const std::string& ref, std::string* path,
                     std::string* note) {
  std::string name;
  if (!MediaRefName(ref, &name)) {
    // 少数包直接写文件路径
    if (zip.Find(ref) != nullptr) {
      *path = ref;
      *note = "清单里写的是字面路径";
      return true;
    }
    return false;
  }

  // <name>.json = layered icon
  for (const char* ext : {".json"}) {
    const std::string candidate = "resources/base/media/" + name + ext;
    if (zip.Find(candidate) != nullptr && ResolveLayered(zip, candidate, path, note)) return true;
  }
  if (FindMediaFile(zip, name, path)) {
    *note = "$media:" + name;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- resources.index（RestoolV2）
// 格式依据：openharmony/developtools_global_resource_tool 的 resource_table.h / resource_table.cpp
// （同一份代码既写也读，字段顺序是权威的）：
//   IndexHeaderV2 {version[128], length, keyCount, dataBlockOffset}
//   KeyConfig     {"KEYS", configId, keyCount, KeyParam[keyCount]}
//   IdSetHeader   {"IDSS", length, typeCount, idCount}
//   ResTypeHeader {resType, length, count}
//   ResIndex      {resId, offset, length, name[length]}
//   ResInfo@offset{resId, length, valueCount} + valueCount × {configId, dataOffset}
//   值 @dataOffset: u16 长度 + UTF-8 字节
// 我们只用它做一件事：把 $string:app_name 解成真名（取默认配置那一条）。
constexpr size_t kV2HeaderLen = 128 + 12;
constexpr size_t kKeyConfigHeaderLen = 12;
constexpr size_t kKeyParamLen = 8;
constexpr size_t kIdSetHeaderLen = 16;
constexpr size_t kResTypeHeaderLen = 12;
constexpr size_t kResIndexLen = 12;
constexpr size_t kResInfoLen = 12;

class IndexReader {
 public:
  IndexReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool Has(size_t offset, size_t length) const {
    return offset <= size_ && length <= size_ - offset;
  }
  bool Tag(size_t offset, const char* tag) const {
    return Has(offset, 4) && std::memcmp(data_ + offset, tag, 4) == 0;
  }
  uint16_t U16(size_t offset) const {
    return static_cast<uint16_t>(data_[offset] | (data_[offset + 1] << 8));
  }
  uint32_t U32(size_t offset) const {
    return static_cast<uint32_t>(data_[offset]) |
           (static_cast<uint32_t>(data_[offset + 1]) << 8) |
           (static_cast<uint32_t>(data_[offset + 2]) << 16) |
           (static_cast<uint32_t>(data_[offset + 3]) << 24);
  }
  std::string Str(size_t offset, size_t length) const {
    return std::string(reinterpret_cast<const char*>(data_ + offset), length);
  }

 private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

// 在 ResIndex 指向的 ResInfo 里挑"默认配置"（keyCount == 0 的 configId）的值
bool ReadDefaultStringValue(const IndexReader& view,
                            const std::vector<uint32_t>& default_configs,
                            size_t res_info_offset, std::string* out) {
  if (!view.Has(res_info_offset, kResInfoLen)) return false;
  const uint32_t value_count = view.U32(res_info_offset + 8);
  size_t cursor = res_info_offset + kResInfoLen;
  uint32_t best_offset = 0;
  int best_rank = 1 << 30;
  bool found = false;
  for (uint32_t i = 0; i < value_count; ++i) {
    if (!view.Has(cursor, 8)) break;
    const uint32_t config_id = view.U32(cursor);
    const uint32_t data_offset = view.U32(cursor + 4);
    cursor += 8;
    const bool is_default =
        std::find(default_configs.begin(), default_configs.end(), config_id) !=
        default_configs.end();
    const int rank = is_default ? 0 : 1;  // 非默认配置只作兜底
    if (rank < best_rank) {
      best_rank = rank;
      best_offset = data_offset;
      found = true;
    }
  }
  if (!found || !view.Has(best_offset, 2)) return false;
  const uint16_t length = view.U16(best_offset);
  if (!view.Has(best_offset + 2, length)) return false;
  *out = view.Str(best_offset + 2, length);
  return true;
}

}  // namespace

bool LooksLikeHap(const ZipReader& zip) {
  if (!zip.IsOpen()) return false;
  if (zip.Find("AndroidManifest.xml") != nullptr) return false;
  return zip.Find("module.json") != nullptr;
}

bool ResolveHapString(const std::vector<uint8_t>& index, uint32_t res_id,
                      const std::string& res_name, std::string* out) {
  if (out == nullptr || index.size() < kV2HeaderLen) return false;
  const IndexReader view(index.data(), index.size());
  const uint32_t key_count = view.U32(128 + 4);
  if (key_count > 4096) return false;

  // 键配置：记下 keyCount == 0 的 configId（默认/无限定词）
  std::vector<uint32_t> default_configs;
  size_t cursor = kV2HeaderLen;
  for (uint32_t i = 0; i < key_count; ++i) {
    if (!view.Has(cursor, kKeyConfigHeaderLen) || !view.Tag(cursor, "KEYS")) return false;
    const uint32_t config_id = view.U32(cursor + 4);
    const uint32_t param_count = view.U32(cursor + 8);
    if (param_count > 4096) return false;
    cursor += kKeyConfigHeaderLen;
    const size_t params_len = static_cast<size_t>(param_count) * kKeyParamLen;
    if (!view.Has(cursor, params_len)) return false;
    cursor += params_len;
    if (param_count == 0) default_configs.push_back(config_id);
  }

  if (!view.Has(cursor, kIdSetHeaderLen) || !view.Tag(cursor, "IDSS")) return false;
  const uint32_t type_count = view.U32(cursor + 8);
  if (type_count > 64) return false;
  cursor += kIdSetHeaderLen;

  for (uint32_t t = 0; t < type_count; ++t) {
    if (!view.Has(cursor, kResTypeHeaderLen)) return false;
    const uint32_t res_type = view.U32(cursor);
    const uint32_t res_count = view.U32(cursor + 8);
    if (res_type > 0xFFFF || res_count > 200000) return false;
    cursor += kResTypeHeaderLen;

    for (uint32_t r = 0; r < res_count; ++r) {
      if (!view.Has(cursor, kResIndexLen)) return false;
      const uint32_t id = view.U32(cursor);
      const uint32_t offset = view.U32(cursor + 4);
      const uint32_t name_len = view.U32(cursor + 8);
      if (!view.Has(cursor + kResIndexLen, name_len)) return false;
      const std::string name = view.Str(cursor + kResIndexLen, name_len);
      cursor += kResIndexLen + name_len;

      // id 唯一（跨 resType 也不重复）；名字命中更明确，优先按名字
      const bool name_matches = !res_name.empty() && name == res_name;
      const bool id_matches = res_id != 0 && id == res_id;
      if (!name_matches && !id_matches) continue;
      if (ReadDefaultStringValue(view, default_configs, offset, out) && !out->empty()) return true;
    }
  }
  return false;
}

bool ReadHapManifest(const ZipReader& zip, HapInfo* out, Error* err) {
  if (out == nullptr) return false;
  *out = HapInfo();

  std::string text;
  if (!ReadEntryText(zip, "module.json", kMaxHapManifest, &text)) {
    if (err) err->set(L"module.json 读不出来");
    return false;
  }
  json::ValuePtr doc;
  Error parse_error;
  if (!json::Parse(text, &doc, &parse_error) || doc == nullptr) {
    if (err) {
      err->set(L"module.json 不是合法 JSON");
      if (!parse_error.empty()) err->message += L"：" + parse_error.message;
    }
    return false;
  }
  const json::Value* app = Field(doc.get(), "app");
  const json::Value* module = Field(doc.get(), "module");
  if (app == nullptr || module == nullptr) {
    if (err) err->set(L"module.json 缺少 app / module 段");
    return false;
  }

  out->bundle_name = StrField(app, "bundleName");
  out->version_name = StrField(app, "versionName");
  out->version_code = UintField(app, "versionCode");
  out->min_api = UintField(app, "minAPIVersion");
  out->target_api = UintField(app, "targetAPIVersion");
  out->compile_sdk = StrField(app, "compileSdkVersion");
  out->icon_ref = StrField(app, "icon");

  // 标签的引用先记下，等 module/abilities 解析完再决定用哪一条（见下）
  const std::string app_label = StrField(app, "label");
  const uint32_t app_label_id = UintField(app, "labelId");
  if (app_label.size() > 8 && app_label.compare(0, 8, "$string:") == 0) {
    out->app_label_ref = app_label;
  }

  out->module_name = StrField(module, "name");
  out->module_type = StrField(module, "type");
  out->main_element = StrField(module, "mainElement");
  out->device_types = StrArrayField(module, "deviceTypes");
  out->abilities = static_cast<uint32_t>(ArraySize(module, "abilities"));
  out->extension_abilities = static_cast<uint32_t>(ArraySize(module, "extensionAbilities"));

  if (const json::Value* perms = Field(module, "requestPermissions")) {
    if (perms->type == json::Type::Array) {
      for (const json::ValuePtr& item : perms->items) {
        const std::string name = StrField(item.get(), "name");
        if (!name.empty()) out->permissions.push_back(name);
      }
    }
  }

  // 鸿蒙把"桌面图标 + 名称"配在 UIAbility 上（abilities[].icon/label），
  // app.label 只是缺省值：优先用 mainElement 那个 ability 的 label。
  std::string label_ref = app_label;
  uint32_t label_id = app_label_id;
  bool have_ability_label = false;
  if (const json::Value* abilities = Field(module, "abilities")) {
    if (abilities->type == json::Type::Array) {
      for (const json::ValuePtr& item : abilities->items) {
        const std::string start = StrField(item.get(), "startWindowIcon");
        if (!start.empty() && out->start_icon_ref.empty()) out->start_icon_ref = start;

        const std::string name = StrField(item.get(), "name");
        const bool is_main = out->main_element.empty() ? !have_ability_label
                                                       : name == out->main_element;
        const std::string ability_label = StrField(item.get(), "label");
        if (is_main && !ability_label.empty() && !have_ability_label) {
          label_ref = ability_label;
          label_id = UintField(item.get(), "labelId");
          have_ability_label = true;
        }
      }
    }
  }

  // 字面量直接用；$string:NAME → 去 resources.index 里取默认配置的值
  if (!label_ref.empty()) {
    if (label_ref.size() > 8 && label_ref.compare(0, 8, "$string:") == 0) {
      out->label_ref = label_ref;
      out->label_id = label_id;
      std::vector<uint8_t> index_bytes;
      const ZipEntry* index_entry = zip.Find("resources.index");
      if (index_entry != nullptr &&
          zip.Read(*index_entry, kMaxHapIndexRead, &index_bytes, nullptr)) {
        std::string value;
        if (ResolveHapString(index_bytes, label_id, label_ref.substr(8), &value)) {
          out->label = value;
        }
      }
    } else {
      out->label = label_ref;
    }
  }

  if (out->bundle_name.empty()) {
    if (err) err->set(L"module.json 里没有 bundleName");
    return false;
  }
  return true;
}

bool ReadHapIcon(const ZipReader& zip, const HapInfo& info, HapIcon* out, Error* err) {
  if (out == nullptr) return false;
  *out = HapIcon();

  std::string path;
  std::string note;
  bool found = false;

  // ① 清单声明的 app.icon（可能是 $media: 名、layered json、或字面路径）
  if (!info.icon_ref.empty()) found = ResolveMediaRef(zip, info.icon_ref, &path, &note);
  // ② 第一个 ability 的启动图标
  if (!found && !info.start_icon_ref.empty()) {
    found = ResolveMediaRef(zip, info.start_icon_ref, &path, &note);
  }
  // ③ 约定名 → ④ base/media 里最大的 PNG
  if (!found) {
    for (const char* name : {"app_icon", "icon", "AppIcon"}) {
      if (FindMediaFile(zip, name, &path)) {
        note = std::string("约定名 ") + name;
        found = true;
        break;
      }
    }
  }
  if (!found) {
    uint64_t best = 0;
    for (const ZipEntry& entry : zip.entries()) {
      const std::string lower = LowerAscii(entry.name);
      if (lower.find("resources/") != 0 || lower.find("/media/") == std::string::npos) continue;
      if (!EndsWithNoCase(lower, ".png")) continue;
      if (entry.uncompressed_size > best) {
        best = entry.uncompressed_size;
        path = entry.name;
      }
    }
    if (best != 0) {
      note = "base/media 里最大的 PNG（没有明确引用）";
      found = true;
    }
  }

  if (!found) {
    if (err) err->set(L"HAP 里找不到可用图标");
    return false;
  }

  const ZipEntry* entry = zip.Find(path);
  if (entry == nullptr || !zip.Read(*entry, kMaxHapIconRead, &out->bytes, nullptr)) {
    if (err) err->set(L"图标条目读不出来");
    return false;
  }
  out->path = path;
  out->note = note;
  if (out->bytes.empty()) {
    if (err) err->set(L"图标条目是空的");
    return false;
  }
  return true;
}

}  // namespace apksx

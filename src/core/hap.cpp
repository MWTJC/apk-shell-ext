#include "core/hap.h"

#include <algorithm>
#include <cctype>

#include "core/json_lite.h"
#include "core/zip_reader.h"

namespace apksx {
namespace {

// module.json 只有一两 KB，给足余量
constexpr uint64_t kMaxHapManifest = 4ull * 1024 * 1024;
constexpr uint64_t kMaxLayeredJson = 64ull * 1024;
constexpr uint64_t kMaxHapIconRead = 16ull * 1024 * 1024;

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

}  // namespace

bool LooksLikeHap(const ZipReader& zip) {
  if (!zip.IsOpen()) return false;
  if (zip.Find("AndroidManifest.xml") != nullptr) return false;
  return zip.Find("module.json") != nullptr;
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

  // 标签：字面量就直接用，$string: 只记引用（v1 不解 resources.index）
  const std::string label = StrField(app, "label");
  if (!label.empty()) {
    if (label.size() > 8 && label.compare(0, 8, "$string:") == 0) {
      out->label_ref = label;
    } else {
      out->label = label;
    }
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

  if (const json::Value* abilities = Field(module, "abilities")) {
    if (abilities->type == json::Type::Array) {
      for (const json::ValuePtr& item : abilities->items) {
        const std::string start = StrField(item.get(), "startWindowIcon");
        if (!start.empty() && out->start_icon_ref.empty()) out->start_icon_ref = start;
      }
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

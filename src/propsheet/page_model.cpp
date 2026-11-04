#include "propsheet/page_model.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>

namespace apksx {
namespace {

std::wstring U8(std::string_view utf8) {
  std::wstring out;
  if (!Utf8ToWide(utf8, &out)) return std::wstring();
  return out;
}

std::wstring JoinStrings(const std::vector<std::string>& items, const wchar_t* sep,
                         size_t max_items) {
  std::wstring out;
  const size_t count = std::min(items.size(), max_items);
  for (size_t i = 0; i < count; ++i) {
    if (!out.empty()) out += sep;
    out += U8(items[i]);
  }
  if (items.size() > count) {
    wchar_t tail[48];
    _snwprintf_s(tail, _TRUNCATE, L"…（共 %zu 项）", items.size());
    out += tail;
  }
  return out;
}

std::wstring FormatBytes(uint64_t bytes) {
  constexpr double kKb = 1024.0;
  constexpr double kMb = kKb * 1024.0;
  constexpr double kGb = kMb * 1024.0;
  wchar_t human[32];
  if (static_cast<double>(bytes) >= kGb) {
    _snwprintf_s(human, _TRUNCATE, L"%.2f GB", static_cast<double>(bytes) / kGb);
  } else if (static_cast<double>(bytes) >= kMb) {
    _snwprintf_s(human, _TRUNCATE, L"%.1f MB", static_cast<double>(bytes) / kMb);
  } else if (static_cast<double>(bytes) >= kKb) {
    _snwprintf_s(human, _TRUNCATE, L"%.1f KB", static_cast<double>(bytes) / kKb);
  } else {
    _snwprintf_s(human, _TRUNCATE, L"%llu B", static_cast<unsigned long long>(bytes));
  }
  // 精确字节数带千位分隔：9,7 MB 这种看不出真实大小，用户常要对照哈希/台账
  std::wstring grouped;
  {
    const std::wstring digits = std::to_wstring(bytes);
    size_t counter = 0;
    for (size_t i = digits.size(); i-- > 0;) {
      grouped.insert(grouped.begin(), digits[i]);
      if (++counter % 3 == 0 && i > 0) grouped.insert(grouped.begin(), L',');
    }
  }
  wchar_t both[96];
  _snwprintf_s(both, _TRUNCATE, L"%s（%s 字节）", human, grouped.c_str());
  return both;
}

std::wstring FormatUnixTime(uint64_t unix_seconds) {
  if (unix_seconds == 0) return std::wstring();
  constexpr uint64_t kEpochDiff = 116444736000000000ull;  // 1601→1970，100ns
  FILETIME ft{};
  const uint64_t ticks = unix_seconds * 10000000ull + kEpochDiff;
  ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFull);
  ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
  FILETIME local{};
  if (!::FileTimeToLocalFileTime(&ft, &local)) return std::wstring();
  SYSTEMTIME st{};
  if (!::FileTimeToSystemTime(&local, &st)) return std::wstring();
  wchar_t buf[40];
  _snwprintf_s(buf, _TRUNCATE, L"%04d-%02d-%02d %02d:%02d:%02d", static_cast<int>(st.wYear),
               static_cast<int>(st.wMonth), static_cast<int>(st.wDay), static_cast<int>(st.wHour),
               static_cast<int>(st.wMinute), static_cast<int>(st.wSecond));
  return buf;
}

const json::Value* Field(const json::Value* obj, const char* key) {
  return obj == nullptr ? nullptr : obj->Find(key);
}

std::wstring StrField(const json::Value* obj, const char* key) {
  const json::Value* v = Field(obj, key);
  return v == nullptr ? std::wstring() : U8(v->as_string());
}

uint32_t UintField(const json::Value* obj, const char* key) {
  const json::Value* v = Field(obj, key);
  return v == nullptr ? 0u : static_cast<uint32_t>(v->as_number(0.0));
}

// 分包可能超过 4 GB（含 obb 的容器），大小用 64 位
uint64_t Uint64Field(const json::Value* obj, const char* key) {
  const json::Value* v = Field(obj, key);
  if (v == nullptr) return 0;
  const double value = v->as_number(0.0);
  return value <= 0.0 ? 0 : static_cast<uint64_t>(value);
}

std::vector<std::string> StringArray(const json::Value* obj, const char* key) {
  std::vector<std::string> out;
  const json::Value* v = Field(obj, key);
  if (v == nullptr || v->type != json::Type::Array) return out;
  for (const json::ValuePtr& item : v->items) {
    if (item != nullptr && item->is_string()) out.push_back(std::string(item->as_string()));
  }
  return out;
}

void AddRow(PageGroup* group, const wchar_t* key, std::wstring value) {
  if (value.empty()) return;
  PageRow row;
  row.key = key;
  row.value = value;
  row.copy_value = value;
  group->rows.push_back(std::move(row));
}

void AddActionRow(PageGroup* group, const wchar_t* key, std::wstring hint) {
  PageRow row;
  row.key = key;
  row.value = std::move(hint);
  row.action = true;
  group->rows.push_back(std::move(row));
}

}  // namespace

bool BuildPageModel(const json::Value& doc, const std::wstring& fallback_name, PageModel* out,
                    Error* err) {
  if (out == nullptr) return false;
  *out = PageModel();
  if (!doc.is_object()) {
    if (err != nullptr) err->set(L"解析输出不是 JSON 对象");
    return false;
  }
  {
    const json::Value* schema = doc.Find("schema");
    out->schema = static_cast<int>(schema == nullptr ? 0.0 : schema->as_number(0.0));
  }

  const json::Value* file = doc.Find("file");
  const json::Value* app = doc.Find("app");
  const json::Value* icon = doc.Find("icon");
  const json::Value* comps = doc.Find("components");
  const json::Value* sig = doc.Find("signature");
  const json::Value* container = doc.Find("container");
  const json::Value* hap = doc.Find("hap");
  const json::Value* diag = doc.Find("diagnostics");

  // ---------------------------------------------------------------- 头部
  out->label = StrField(app, "label");
  if (out->label.empty()) out->label = fallback_name;
  out->package = StrField(app, "package");

  const std::wstring version_name = StrField(app, "versionName");
  const uint32_t version_code = UintField(app, "versionCode");
  const uint32_t min_sdk = UintField(app, "minSdk");
  const uint32_t target_sdk = UintField(app, "targetSdk");

  std::wstring version_value = version_name;
  if (version_code != 0) {
    wchar_t buf[32];
    _snwprintf_s(buf, _TRUNCATE, L" (%u)", version_code);
    version_value += buf;
  }
  {
    std::wstring line = version_value;
    if (min_sdk != 0 || target_sdk != 0) {
      wchar_t buf[64];
      _snwprintf_s(buf, _TRUNCATE, L"minSdk %u / targetSdk %u", min_sdk, target_sdk);
      if (!line.empty()) line += L" · ";
      line += buf;
    }
    out->version_line = line;
  }

  // ---------------------------------------------------------------- 应用
  PageGroup apply;
  apply.title = L"应用";
  AddRow(&apply, L"应用名", out->label);
  AddRow(&apply, L"包名", out->package);
  AddRow(&apply, L"版本", version_value);
  {
    const std::vector<std::string> locales = StringArray(app, "locales");
    if (!locales.empty()) {
      wchar_t head[48];
      _snwprintf_s(head, _TRUNCATE, L"%zu 种 · ", locales.size());
      AddRow(&apply, L"语言", std::wstring(head) + JoinStrings(locales, L", ", 6));
    }
  }
  if (!apply.rows.empty()) out->groups.push_back(std::move(apply));

  // ---------------------------------------------------------------- 技术
  PageGroup tech;
  tech.title = L"技术";
  {
    // 鸿蒙 HAP（M4d）：模块/API/编译 SDK/设备类型取代 APK 那套 SDK 语义
    if (hap != nullptr && hap->is_object()) {
      const std::wstring module = StrField(hap, "module");
      const std::wstring type = StrField(hap, "type");
      if (!module.empty()) {
        AddRow(&tech, L"模块", type.empty() ? module : module + L"（" + type + L"）");
      }
      const std::wstring main_ability = StrField(hap, "mainAbility");
      if (!main_ability.empty()) AddRow(&tech, L"主 Ability", main_ability);
      const uint32_t min_api = UintField(hap, "minAPI");
      const uint32_t target_api = UintField(hap, "targetAPI");
      if (min_api != 0 || target_api != 0) {
        wchar_t buf[80];
        _snwprintf_s(buf, _TRUNCATE, L"min %u / target %u", min_api, target_api);
        AddRow(&tech, L"API 版本", buf);
      }
      const std::wstring compile_sdk = StrField(hap, "compileSdk");
      if (!compile_sdk.empty()) AddRow(&tech, L"编译 SDK", compile_sdk);
      const std::vector<std::string> devices = StringArray(hap, "deviceTypes");
      if (!devices.empty()) AddRow(&tech, L"设备类型", JoinStrings(devices, L", ", 8));
      const uint32_t abilities = UintField(hap, "abilities");
      const uint32_t extensions = UintField(hap, "extensionAbilities");
      if (abilities + extensions > 0) {
        wchar_t buf[64];
        if (extensions == 0) {
          _snwprintf_s(buf, _TRUNCATE, L"Ability %u", abilities);
        } else {
          _snwprintf_s(buf, _TRUNCATE, L"Ability %u · Extension %u", abilities, extensions);
        }
        AddRow(&tech, L"组件", buf);
      }
    }
    const std::vector<std::string> abis = StringArray(app, "abis");
    if (!abis.empty()) AddRow(&tech, L"ABI", JoinStrings(abis, L", ", 6));
    const std::vector<std::string> densities = StringArray(app, "densities");
    if (!densities.empty()) AddRow(&tech, L"密度", JoinStrings(densities, L", ", 8));
  }
  {
    const uint32_t activities = UintField(comps, "activities");
    const uint32_t services = UintField(comps, "services");
    const uint32_t receivers = UintField(comps, "receivers");
    const uint32_t providers = UintField(comps, "providers");
    const bool is_hap = hap != nullptr && hap->is_object();
    if (!is_hap && activities + services + receivers + providers > 0) {
      wchar_t buf[128];
      _snwprintf_s(buf, _TRUNCATE, L"活动 %u · 服务 %u · 广播 %u · 提供者 %u", activities, services,
                   receivers, providers);
      AddRow(&tech, L"组件", buf);
    }
  }
  {
    const std::wstring source = StrField(icon, "source");
    if (!source.empty()) {
      AddRow(&tech, L"图标来源", source);
      const uint32_t width = UintField(icon, "width");
      const uint32_t height = UintField(icon, "height");
      if (width != 0 && height != 0) {
        wchar_t buf[48];
        _snwprintf_s(buf, _TRUNCATE, L"%u × %u", width, height);
        AddRow(&tech, L"图标尺寸", buf);
      }
      const json::Value* adaptive = Field(icon, "adaptive");
      if (adaptive != nullptr && adaptive->as_bool(false)) {
        AddRow(&tech, L"图标类型", L"自适应（XML）");
      }
    }
  }
  if (!tech.rows.empty()) out->groups.push_back(std::move(tech));

  // ---------------------------------------------------------------- 权限（可折叠）
  {
    const json::Value* list = doc.Find("permissions");
    std::vector<std::wstring> names;
    if (list != nullptr && list->type == json::Type::Array) {
      for (const json::ValuePtr& item : list->items) {
        const std::wstring name = StrField(item.get(), "name");
        if (!name.empty()) names.push_back(name);
      }
    }
    if (!names.empty()) {
      PageGroup perms;
      wchar_t buf[64];
      _snwprintf_s(buf, _TRUNCATE, L"权限 · %zu 项", names.size());
      perms.title = buf;
      perms.collapsible = true;  // 分组带带折叠箭头，收起后整段明细隐藏
      for (const std::wstring& name : names) AddRow(&perms, L"", name);
      out->groups.push_back(std::move(perms));
    }
  }

  // ---------------------------------------------------------------- 特性（可折叠）
  {
    const json::Value* list = doc.Find("features");
    std::vector<std::wstring> lines;
    size_t required = 0;
    if (list != nullptr && list->type == json::Type::Array) {
      for (const json::ValuePtr& item : list->items) {
        const std::wstring name = StrField(item.get(), "name");
        if (name.empty()) continue;
        const json::Value* required_field = Field(item.get(), "required");
        const bool is_required = required_field == nullptr || required_field->as_bool(true);
        if (is_required) ++required;
        lines.push_back(name + (is_required ? L"（必需）" : L"（可选）"));
      }
    }
    if (!lines.empty()) {
      PageGroup features;
      wchar_t buf[80];
      _snwprintf_s(buf, _TRUNCATE, L"特性 · %zu 项（%zu 必需）", lines.size(), required);
      features.title = buf;
      features.collapsible = true;
      for (const std::wstring& line : lines) AddRow(&features, L"", line);
      out->groups.push_back(std::move(features));
    }
  }

  // ---------------------------------------------------------------- 分包（可折叠，M4a）
  // 只有 .xapk/.apks/.apkm 这类多 APK 容器才有：base 之外的 config/feature 包列在这里。
  if (container != nullptr && container->is_object()) {
    std::vector<std::wstring> lines;
    const json::Value* list = Field(container, "splits");
    if (list != nullptr && list->type == json::Type::Array) {
      for (const json::ValuePtr& item : list->items) {
        const std::wstring name = StrField(item.get(), "name");
        if (name.empty()) continue;
        const uint64_t size = Uint64Field(item.get(), "size");
        std::wstring kind = StrField(item.get(), "kind");
        if (kind == L"base") {
          kind = L"base";
        } else if (kind == L"config") {
          kind = L"配置";
        } else {
          kind = L"分包";
        }
        std::wstring line = L"[" + kind + L"] " + name;
        if (size != 0) line += L"（" + FormatBytes(size) + L"）";
        lines.push_back(line);
      }
    }
    if (!lines.empty()) {
      PageGroup splits;
      wchar_t buf[64];
      _snwprintf_s(buf, _TRUNCATE, L"分包 · %zu 项", lines.size());
      splits.title = buf;
      splits.collapsible = true;
      for (const std::wstring& line : lines) AddRow(&splits, L"", line);
      out->groups.push_back(std::move(splits));
    }
  }

  // ---------------------------------------------------------------- 文件与签名
  PageGroup files;
  files.title = L"文件与签名";
  {
    // 容器类型放最前：.xapk/.apks/.apkm/.hap 时先让人知道"这不是普通 APK"
    if (hap != nullptr && hap->is_object()) {
      AddRow(&files, L"安装包类型", L"HAP（鸿蒙）");
    }
    if (container != nullptr && container->is_object()) {
      const std::wstring base = StrField(container, "base");
      if (!base.empty()) AddRow(&files, L"内层 APK", base);
      const uint32_t count = UintField(container, "apkCount");
      const std::wstring kind = StrField(container, "kind");
      if (!kind.empty()) {
        std::wstring title = kind;
        for (wchar_t& c : title) c = static_cast<wchar_t>(::towupper(c));
        wchar_t buf[64];
        if (count != 0) {
          _snwprintf_s(buf, _TRUNCATE, L"%s（%u 个 APK）", title.c_str(), count);
          AddRow(&files, L"安装包类型", buf);
        } else {
          AddRow(&files, L"安装包类型", title);
        }
      }
    }
    const uint32_t size = UintField(file, "size");
    if (size != 0) AddRow(&files, L"大小", FormatBytes(size));
    const uint32_t mtime = UintField(file, "mtime");
    if (mtime != 0) AddRow(&files, L"修改时间", FormatUnixTime(mtime));

    const std::wstring sha = StrField(file, "sha256");
    if (sha.empty()) {
      AddActionRow(&files, L"SHA-256", L"未计算");
    } else {
      AddRow(&files, L"SHA-256", sha);
    }
  }
  {
    const bool is_hap = hap != nullptr && hap->is_object();
    if (sig == nullptr) {
      // 用户只关心"这个功能有没有"：没实现的功能一律写"未实现"，不解释原因。
      AddRow(&files, L"签名", is_hap ? L"未实现" : L"未发现签名（可能未签名或被重打包）");
    } else {
      const std::vector<std::string> schemes = StringArray(sig, "scheme");
      if (!schemes.empty()) AddRow(&files, L"签名", JoinStrings(schemes, L", ", 8));
      const json::Value* certs = Field(sig, "certs");
      if (certs != nullptr && certs->type == json::Type::Array && !certs->items.empty()) {
        const json::Value* first = certs->items.front().get();
        AddRow(&files, L"证书", StrField(first, "subject"));
        AddRow(&files, L"证书摘要", StrField(first, "sha256"));
        const std::wstring from = StrField(first, "notBefore");
        const std::wstring to = StrField(first, "notAfter");
        if (!from.empty() || !to.empty()) AddRow(&files, L"有效期", from + L" → " + to);
        if (certs->items.size() > 1) {
          wchar_t buf[64];
          _snwprintf_s(buf, _TRUNCATE, L"另有 %zu 张证书", certs->items.size() - 1);
          AddRow(&files, L"其它证书", buf);
        }
      }
    }
  }
  if (!files.rows.empty()) out->groups.push_back(std::move(files));

  // ---------------------------------------------------------------- 警告
  const json::Value* warnings = Field(diag, "warnings");
  if (warnings != nullptr && warnings->type == json::Type::Array) {
    for (const json::ValuePtr& item : warnings->items) {
      if (item != nullptr && item->is_string()) out->warnings.push_back(U8(item->as_string()));
    }
  }
  return true;
}

void BuildErrorModel(const std::wstring& title, const std::vector<std::wstring>& lines,
                     PageModel* out) {
  if (out == nullptr) return;
  *out = PageModel();
  out->label = title;
  PageGroup group;
  group.title = L"详情";
  for (const std::wstring& line : lines) {
    if (line.empty()) continue;
    PageRow row;
    row.value = line;  // key 留空：绘制时不画键列
    row.copy_value = line;
    group.rows.push_back(std::move(row));
  }
  out->groups.push_back(std::move(group));
}

void SetSha256(PageModel* model, const std::wstring& hex) {
  if (model == nullptr) return;
  for (PageGroup& group : model->groups) {
    for (PageRow& row : group.rows) {
      if (row.key == L"SHA-256") {
        row.value = hex;
        row.copy_value = hex;
        row.action = false;
        return;
      }
    }
  }
}

const PageRow* FindRow(const PageModel& model, const std::wstring& key) {
  for (const PageGroup& group : model.groups) {
    for (const PageRow& row : group.rows) {
      if (row.key == key) return &row;
    }
  }
  return nullptr;
}

}  // namespace apksx

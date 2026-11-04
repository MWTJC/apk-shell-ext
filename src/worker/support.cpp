#include "worker/support.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>

#include "core/file_util.h"
#include "core/i18n.h"
#include "version.h"

namespace apksx {
namespace {

constexpr wchar_t kThumbClsid[] = L"{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B01}";
constexpr wchar_t kPropClsid[] = L"{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B02}";
constexpr wchar_t kThumbHandlerId[] = L"{E357FCCD-A995-4576-B01F-234630154E96}";
constexpr wchar_t kPropHandlerName[] = L"ApkInfo";
// Inno Setup 的 AppId 卸载条目（{AppId}_is1），见 installer\apk-shell-ext.iss
constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
    L"{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B03}_is1";
constexpr wchar_t kDebugFlagName[] = L"apk-shell-ext.debug.on";

constexpr wchar_t kExtensions[][8] = {L"apk", L"xapk", L"apks", L"apkm", L"hap"};

// 控制台用 WriteConsoleW（UNICODE），重定向到管道时退回 UTF-8
void Print(const wchar_t* fmt, ...) {
  wchar_t buffer[2048];
  va_list args;
  va_start(args, fmt);
  _vsnwprintf_s(buffer, _TRUNCATE, fmt, args);
  va_end(args);

  HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (out != INVALID_HANDLE_VALUE && out != nullptr && ::GetConsoleMode(out, &mode) != 0) {
    DWORD written = 0;
    ::WriteConsoleW(out, buffer, static_cast<DWORD>(wcslen(buffer)), &written, nullptr);
    return;
  }
  char utf8[8192] = {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8, sizeof(utf8) - 1, nullptr,
                                      nullptr);
  if (n > 0) {
    DWORD written = 0;
    ::WriteFile(out, utf8, static_cast<DWORD>(n - 1), &written, nullptr);
  }
}

bool KeyExists(HKEY root, const std::wstring& subkey) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) !=
      ERROR_SUCCESS) {
    return false;
  }
  ::RegCloseKey(key);
  return true;
}

bool ReadString(HKEY root, const std::wstring& subkey, std::wstring* out) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) !=
      ERROR_SUCCESS) {
    return false;
  }
  DWORD type = 0;
  DWORD bytes = 0;
  bool ok = false;
  if (::RegQueryValueExW(key, nullptr, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
      (type == REG_SZ || type == REG_EXPAND_SZ) && bytes >= sizeof(wchar_t)) {
    std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (::RegQueryValueExW(key, nullptr, nullptr, &type,
                           reinterpret_cast<LPBYTE>(value.data()), &bytes) == ERROR_SUCCESS) {
      value.resize(wcslen(value.c_str()));
      *out = std::move(value);
      ok = true;
    }
  }
  ::RegCloseKey(key);
  return ok;
}

std::wstring ClassesKey(const wchar_t* leaf) {
  std::wstring key = L"Software\\Classes\\";
  key += leaf;
  return key;
}

// 带值名的读（不指定名字 = 默认值）。--status 与 --features 都要读具名值。
std::wstring Join(const std::wstring& dir, const wchar_t* leaf);

bool ReadStringNamed(HKEY root, const std::wstring& subkey, const wchar_t* name,
                     std::wstring* out) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) !=
      ERROR_SUCCESS) {
    return false;
  }
  DWORD type = 0;
  DWORD bytes = 0;
  bool ok = false;
  if (::RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
      (type == REG_SZ || type == REG_EXPAND_SZ) && bytes >= sizeof(wchar_t)) {
    std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(value.data()),
                           &bytes) == ERROR_SUCCESS) {
      value.resize(wcslen(value.c_str()));
      *out = std::move(value);
      ok = true;
    }
  }
  ::RegCloseKey(key);
  return ok;
}

bool WriteStringNamed(HKEY root, const std::wstring& subkey, const wchar_t* name,
                      const std::wstring& value) {
  HKEY key = nullptr;
  DWORD disposition = 0;
  if (::RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, &disposition) !=
      ERROR_SUCCESS) {
    return false;
  }
  const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  const LSTATUS status = ::RegSetValueExW(key, name, 0, REG_SZ,
                                          reinterpret_cast<const BYTE*>(value.c_str()), bytes);
  ::RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

// 删一个值：本来就不存在也算成功（幂等）。
bool DeleteValueNamed(HKEY root, const std::wstring& subkey, const wchar_t* name) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, subkey.c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) !=
      ERROR_SUCCESS) {
    return true;
  }
  const LSTATUS status = ::RegDeleteValueW(key, name);
  ::RegCloseKey(key);
  return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

std::wstring InstallDir() {
  wchar_t path[MAX_PATH * 2] = {};
  const DWORD n = ::GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
  if (n == 0 || n >= ARRAYSIZE(path)) return std::wstring();
  std::wstring full(path, n);
  const size_t pos = full.find_last_of(L"\\/");
  return pos == std::wstring::npos ? std::wstring() : full.substr(0, pos);
}

// 扩展名 -> 该用哪枚静态图标（与 installer 的 DefaultIcon 声明保持一致）
const wchar_t* IconFileFor(const wchar_t* ext) {
  if (::_wcsicmp(ext, L"apk") == 0) return L"apk.ico";
  if (::_wcsicmp(ext, L"hap") == 0) return L"hap.ico";
  return L"apks.ico";
}

std::wstring ExpectedIconValue(const std::wstring& dir, const wchar_t* ext) {
  return L"\"" + Join(Join(dir, L"icons"), IconFileFor(ext)) + L"\",0";
}

// DefaultIcon 的值形如 `"<install dir>\icons\apk.ico",0`。判断"是不是我们的"不能死比绝对路径：
// 开发态（tools\register.ps1 就地注册 out\x64）和安装态（%LOCALAPPDATA%\Programs\…）目录不同，
// 却都是我们自己写的。所以只认"路径以 \icons\<该扩展名的 ico> 结尾"且文件确实存在。
bool IsOurIconValue(const std::wstring& value, const wchar_t* ext) {
  std::wstring path = value;
  const size_t comma = path.find_last_of(L',');
  if (comma != std::wstring::npos) path.resize(comma);
  while (!path.empty() && (path.front() == L'"' || path.front() == L' ')) path.erase(0, 1);
  while (!path.empty() && (path.back() == L'"' || path.back() == L' ')) path.pop_back();
  if (path.empty()) return false;

  std::wstring tail = L"\\icons\\";
  tail += IconFileFor(ext);
  if (path.size() < tail.size()) return false;
  if (::_wcsicmp(path.c_str() + (path.size() - tail.size()), tail.c_str()) != 0) return false;
  return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}


// 一个扩展名上两个"功能开关"的现状
struct ExtFeature {
  bool thumb_on = false;         // ShellEx\{E357FCCD} 指向我们的 CLSID
  bool icon_on = false;          // DefaultIcon 指向安装目录里的 ico
  std::wstring thumb_owner;      // 非空 = 被别人占着（值内容）
  std::wstring icon_owner;
};

void ProbeExtension(const wchar_t* ext, ExtFeature* out) {
  const std::wstring base = std::wstring(L"Software\\Classes\\.") + ext;
  const std::wstring thumb_key = base + L"\\ShellEx\\" + kThumbHandlerId;
  const std::wstring icon_key = base + L"\\DefaultIcon";
  std::wstring value;

  if (ReadStringNamed(HKEY_CURRENT_USER, thumb_key, nullptr, &value) ||
      ReadStringNamed(HKEY_LOCAL_MACHINE, thumb_key, nullptr, &value)) {
    if (::_wcsicmp(value.c_str(), kThumbClsid) == 0) {
      out->thumb_on = true;
    } else {
      out->thumb_owner = value;
    }
  }
  if (ReadStringNamed(HKEY_CURRENT_USER, icon_key, nullptr, &value) ||
      ReadStringNamed(HKEY_LOCAL_MACHINE, icon_key, nullptr, &value)) {
    if (IsOurIconValue(value, ext)) {
      out->icon_on = true;
    } else {
      out->icon_owner = value;
    }
  }
}


std::wstring Join(const std::wstring& dir, const wchar_t* leaf) {
  if (dir.empty()) return leaf;
  std::wstring out = dir;
  if (out.back() != L'\\') out.push_back(L'\\');
  out += leaf;
  return out;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr;
  std::wstring out;
  if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) && raw != nullptr) {
    out = raw;
    ::CoTaskMemFree(raw);
  }
  return out;
}

}  // namespace

int RunStatusCommand() {
  Print(L"%ls %ls\n", L"" APKSX_PRODUCT_NAME, L"" APKSX_VERSION_FULL);

  const std::wstring user_dir =
      Join(KnownFolder(FOLDERID_LocalAppData), L"Programs\\apk-shell-ext");
  const std::wstring machine_dir = Join(KnownFolder(FOLDERID_ProgramFiles), L"apk-shell-ext");
  const auto present = [](const std::wstring& dir) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    return ::GetFileAttributesExW(Join(dir, L"apkthumb.dll").c_str(), GetFileExInfoStandard,
                                  &fad) != FALSE
               ? L"已安装"
               : L"未安装";
  };
  Print(L"  用户目录：%ls（%ls）\n", user_dir.c_str(), present(user_dir));
  Print(L"  整机目录：%ls（%ls）\n", machine_dir.c_str(), present(machine_dir));

  for (const auto& ext : kExtensions) {
    std::wstring thumb, thumb_machine, prop, icon;
    std::wstring key = ClassesKey(L".");
    key += ext;
    const bool user_thumb = ReadString(HKEY_CURRENT_USER, key + L"\\ShellEx\\" + kThumbHandlerId, &thumb);
    const bool machine_thumb =
        ReadString(HKEY_LOCAL_MACHINE, key + L"\\ShellEx\\" + kThumbHandlerId, &thumb_machine);
    const bool user_prop = ReadString(
        HKEY_CURRENT_USER, key + L"\\shellex\\PropertySheetHandlers\\" + kPropHandlerName, &prop);
    const bool user_icon = ReadString(HKEY_CURRENT_USER, key + L"\\DefaultIcon", &icon);
    Print(L"  .%-5ls thumb=%ls prop=%ls\n", ext,
          user_thumb ? L"用户" : (machine_thumb ? L"整机" : L"-"), user_prop ? L"用户" : L"-");
    if (user_icon) Print(L"          icon=%ls\n", icon.c_str());
  }

  // 卸载入口是否存在（Inno 的 AppId 见 installer\apk-shell-ext.iss）
  const bool has_uninstall = KeyExists(HKEY_CURRENT_USER, kUninstallKey) ||
                             KeyExists(HKEY_LOCAL_MACHINE, kUninstallKey);
  Print(L"  应用列表条目：%ls\n", has_uninstall ? L"有" : L"无");

  std::wstring data;
  if (GetDataDir(&data)) Print(L"  数据目录：%ls\n", data.c_str());
  Print(L"  （缓存清理：apkinfo --clear-cache；诊断开关：apkinfo --diag on|off|status）\n");
  Print(L"  （功能开关：apkinfo --features；关缩略图 thumb=off、关静态图标 icon=off）\n");
  return 0;
}

// apkinfo --lang zh|en|auto：把界面语言写到 HKCU\Software\apk-shell-ext\UiLanguage。
// 属性页跑在 explorer.exe 里，环境变量传不进去，所以必须落到注册表（ADR-0007）。
int RunLangCommand(const std::wstring& action) {
  const std::wstring key_path = L"Software\\apk-shell-ext";
  if (action == L"auto") {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, KEY_SET_VALUE, &key) ==
        ERROR_SUCCESS) {
      ::RegDeleteValueW(key, L"UiLanguage");
      ::RegCloseKey(key);
    }
    Print(L"[lang] 已恢复为跟随系统（当前：%ls）\n", UiLanguage() == Lang::Zh ? L"中文" : L"English");
    return 0;
  }
  if (action != L"zh" && action != L"en") {
    Print(L"[lang] 不认识的语言：%ls（用 zh / en / auto）\n", action.c_str());
    return 4;
  }
  HKEY key = nullptr;
  DWORD disposition = 0;
  if (::RegCreateKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key,
                        &disposition) != ERROR_SUCCESS) {
    Print(L"[lang] 无法写 %ls（error %lu）\n", key_path.c_str(), ::GetLastError());
    return 4;
  }
  const DWORD bytes = static_cast<DWORD>((action.size() + 1) * sizeof(wchar_t));
  const LSTATUS status = ::RegSetValueExW(key, L"UiLanguage", 0, REG_SZ,
                                          reinterpret_cast<const BYTE*>(action.c_str()), bytes);
  ::RegCloseKey(key);
  if (status != ERROR_SUCCESS) {
    Print(L"[lang] 写入失败（%ld）\n", status);
    return 4;
  }
  SetUiLanguage(action == L"zh" ? Lang::Zh : Lang::En);
  Print(L"[lang] 已设为 %ls；重启资源管理器后属性页生效\n",
        action == L"zh" ? L"中文" : L"English");
  return 0;
}

// ---- 功能开关（ADR-0008）：缩略图 / 静态图标可关，属性页恒开 -------------------------
// 只动"扩展名表面"的 15 个值。关闭时只删"指向我们自己"的值，别人的值原样留着并如实报告；
// "我们装好之后又被别人接管"再卸载的那种情况不管（用户 2026-11-04 明确排除）。
namespace {

HKEY TargetRoot() {
  std::wstring value;
  const std::wstring key =
      L"Software\\Classes\\CLSID\\" + std::wstring(kThumbClsid) + L"\\InprocServer32";
  if (ReadStringNamed(HKEY_LOCAL_MACHINE, key, nullptr, &value)) return HKEY_LOCAL_MACHINE;
  return HKEY_CURRENT_USER;
}

bool EnableThumbs(HKEY root, const std::wstring& dir) {
  const std::wstring clsid = L"Software\\Classes\\CLSID\\" + std::wstring(kThumbClsid);
  bool ok = WriteStringNamed(root, clsid, nullptr, L"APK Thumbnail Provider (apk-shell-ext)");
  ok = WriteStringNamed(root, clsid + L"\\InprocServer32", nullptr, Join(dir, L"apkthumb.dll")) && ok;
  ok = WriteStringNamed(root, clsid + L"\\InprocServer32", L"ThreadingModel", L"Apartment") && ok;
  for (const auto& ext : kExtensions) {
    const std::wstring key =
        std::wstring(L"Software\\Classes\\.") + ext + L"\\ShellEx\\" + kThumbHandlerId;
    ok = WriteStringNamed(root, key, nullptr, kThumbClsid) && ok;
  }
  return ok;
}

bool EnableIcons(HKEY root, const std::wstring& dir) {
  bool ok = true;
  for (const auto& ext : kExtensions) {
    const std::wstring file = Join(Join(dir, L"icons"), IconFileFor(ext));
    if (::GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
      Print(L"[features] 找不到 %ls；图标功能只在安装目录里开（开发态用 tools\\register.ps1）\n",
            file.c_str());
      return false;
    }
    const std::wstring key = std::wstring(L"Software\\Classes\\.") + ext + L"\\DefaultIcon";
    ok = WriteStringNamed(root, key, nullptr, ExpectedIconValue(dir, ext)) && ok;
  }
  return ok;
}

// 删掉空壳键（等价 Inno 的 uninsdeletekeyifempty）：值删了但键还在 → 快照里算残留。
void DeleteKeyIfEmpty(HKEY root, const std::wstring& subkey) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) !=
      ERROR_SUCCESS) {
    return;
  }
  DWORD subkeys = 0;
  DWORD values = 0;
  const bool empty =
      ::RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values,
                         nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS &&
      subkeys == 0 && values == 0;
  ::RegCloseKey(key);
  if (empty) ::RegDeleteKeyW(root, subkey.c_str());
}

// 只删"我们的"值；值属于别人就把 owner 带出来（报告用，不动它）。
bool ReleaseOne(const std::wstring& subkey, const std::wstring& expected,
                std::wstring* owner) {
  std::wstring value;
  if (ReadStringNamed(HKEY_CURRENT_USER, subkey, nullptr, &value)) {
    if (::_wcsicmp(value.c_str(), expected.c_str()) == 0) {
      const bool ok = DeleteValueNamed(HKEY_CURRENT_USER, subkey, nullptr);
      DeleteKeyIfEmpty(HKEY_CURRENT_USER, subkey);
      return ok;
    }
    *owner = value;
    return true;
  }
  if (ReadStringNamed(HKEY_LOCAL_MACHINE, subkey, nullptr, &value)) {
    if (::_wcsicmp(value.c_str(), expected.c_str()) == 0) {
      const bool ok = DeleteValueNamed(HKEY_LOCAL_MACHINE, subkey, nullptr);
      DeleteKeyIfEmpty(HKEY_LOCAL_MACHINE, subkey);
      return ok;
    }
    *owner = value;
    return true;
  }
  return true;
}

// DefaultIcon 的对应版本：判断用 IsOurIconValue（认 \icons\<file>，不认死路径）
bool ReleaseIcon(const std::wstring& subkey, const wchar_t* ext, std::wstring* owner) {
  std::wstring value;
  if (ReadStringNamed(HKEY_CURRENT_USER, subkey, nullptr, &value)) {
    if (IsOurIconValue(value, ext)) {
      const bool ok = DeleteValueNamed(HKEY_CURRENT_USER, subkey, nullptr);
      DeleteKeyIfEmpty(HKEY_CURRENT_USER, subkey);
      return ok;
    }
    *owner = value;
    return true;
  }
  if (ReadStringNamed(HKEY_LOCAL_MACHINE, subkey, nullptr, &value)) {
    if (IsOurIconValue(value, ext)) {
      const bool ok = DeleteValueNamed(HKEY_LOCAL_MACHINE, subkey, nullptr);
      DeleteKeyIfEmpty(HKEY_LOCAL_MACHINE, subkey);
      return ok;
    }
    *owner = value;
    return true;
  }
  return true;
}

void PrintFeatureStatus() {
  unsigned thumb_on = 0;
  unsigned icon_on = 0;
  std::wstring lines;
  for (const auto& ext : kExtensions) {
    ExtFeature f;
    ProbeExtension(ext, &f);
    if (f.thumb_on) ++thumb_on;
    if (f.icon_on) ++icon_on;
    lines += L"  .";
    lines += ext;
    for (size_t i = wcslen(ext); i < 6; ++i) lines += L' ';
    lines += f.thumb_on ? L"thumb=开"
                        : (f.thumb_owner.empty() ? L"thumb=关" : L"thumb=其他程序");
    lines += f.icon_on ? L"  icon=开"
                       : (f.icon_owner.empty() ? L"  icon=关" : L"  icon=其他程序");
    lines += L"\n";
  }
  Print(L"[features] thumb=%u/5 icon=%u/5 prop=5/5（核心功能，不可关）\n", thumb_on, icon_on);
  Print(L"%ls", lines.c_str());
}

}  // namespace

int RunFeaturesCommand(const std::wstring& raw_action) {
  const std::wstring dir = InstallDir();
  if (dir.empty()) {
    Print(L"[features] 无法定位安装目录\n");
    return 4;
  }

  std::wstring action = raw_action;
  while (!action.empty() && (action.front() == L' ' || action.front() == L'\t')) action.erase(0, 1);
  while (!action.empty() && (action.back() == L' ' || action.back() == L'\t')) action.pop_back();
  for (wchar_t& c : action) c = static_cast<wchar_t>(::towlower(c));

  if (action.empty() || action == L"status") {
    PrintFeatureStatus();
    return 0;
  }

  bool have_thumbs = false, want_thumbs = false;
  bool have_icons = false, want_icons = false;
  size_t pos = 0;
  while (pos < action.size()) {
    const size_t comma = action.find(L',', pos);
    const std::wstring token =
        action.substr(pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos);
    pos = (comma == std::wstring::npos) ? action.size() : comma + 1;
    if (token.empty()) continue;

    const size_t eq = token.find(L'=');
    if (eq == std::wstring::npos) {
      Print(L"[features] 看不懂 %ls（要 key=on|off）\n", token.c_str());
      return 4;
    }
    const std::wstring key = token.substr(0, eq);
    const std::wstring value = token.substr(eq + 1);
    if (value != L"on" && value != L"off") {
      Print(L"[features] %ls 只接受 on / off\n", key.c_str());
      return 4;
    }
    const bool on = (value == L"on");
    if (key == L"prop" || key == L"props" || key == L"propsheet") {
      if (!on) {
        Print(L"[features] 属性页是核心功能，不能关闭；不需要请卸载安装包\n");
        return 4;
      }
      continue;  // prop=on 就是恒态，不写注册表
    }
    if (key == L"all") {
      have_thumbs = have_icons = true;
      want_thumbs = want_icons = on;
    } else if (key == L"thumb" || key == L"thumbs") {
      have_thumbs = true;
      want_thumbs = on;
    } else if (key == L"icon" || key == L"icons") {
      have_icons = true;
      want_icons = on;
    } else {
      Print(L"[features] 不认识的功能 %ls（可用 thumb / icon / all / prop）\n", key.c_str());
      return 4;
    }
  }

  const HKEY root = TargetRoot();
  bool ok = true;
  if (have_thumbs) {
    if (want_thumbs) {
      ok = EnableThumbs(root, dir) && ok;
    } else {
      for (const auto& ext : kExtensions) {
        const std::wstring key =
            std::wstring(L"Software\\Classes\\.") + ext + L"\\ShellEx\\" + kThumbHandlerId;
        std::wstring owner;
        if (!ReleaseOne(key, kThumbClsid, &owner)) {
          Print(L"[features] 删除 .%ls 的缩略图注册失败\n", ext);
          ok = false;
        } else if (!owner.empty()) {
          Print(L"[features] .%ls 的缩略图由其他程序提供（%ls），保持原样\n", ext, owner.c_str());
        }
      }
    }
  }
  if (have_icons) {
    if (want_icons) {
      ok = EnableIcons(root, dir) && ok;
    } else {
      for (const auto& ext : kExtensions) {
        const std::wstring key = std::wstring(L"Software\\Classes\\.") + ext + L"\\DefaultIcon";
        std::wstring owner;
        if (!ReleaseIcon(key, ext, &owner)) {
          Print(L"[features] 删除 .%ls 的图标注册失败\n", ext);
          ok = false;
        } else if (!owner.empty()) {
          Print(L"[features] .%ls 的图标由其他程序提供（%ls），保持原样\n", ext, owner.c_str());
        }
      }
    }
  }

  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  PrintFeatureStatus();
  Print(L"[features] 资源管理器通常自己会刷新；图标缓存没跟上的话重启一次 explorer\n");
  return ok ? 0 : 4;
}


int RunDiagCommand(const std::wstring& action) {
  wchar_t temp[MAX_PATH * 2] = {};
  const DWORD n = ::GetTempPathW(ARRAYSIZE(temp), temp);
  if (n == 0 || n >= ARRAYSIZE(temp)) return 4;
  const std::wstring flag = std::wstring(temp) + kDebugFlagName;

  std::wstring data;
  std::wstring log;
  if (GetDataDir(&data)) log = Join(data, L"apkthumb.log");

  if (action == L"on") {
    HANDLE h = ::CreateFileW(flag.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      Print(L"[diag] 无法创建 %ls（error %lu）\n", flag.c_str(), ::GetLastError());
      return 4;
    }
    ::CloseHandle(h);
    Print(L"[diag] 已开启：%ls\n", flag.c_str());
    Print(L"[diag] 日志：%ls（重启 explorer 后生效）\n", log.c_str());
    return 0;
  }
  if (action == L"off") {
    if (!::DeleteFileW(flag.c_str()) && ::GetLastError() != ERROR_FILE_NOT_FOUND) {
      Print(L"[diag] 无法删除 %ls（error %lu）\n", flag.c_str(), ::GetLastError());
      return 4;
    }
    Print(L"[diag] 已关闭\n");
    return 0;
  }
  if (action == L"clear") {
    if (!::DeleteFileW(log.c_str()) && ::GetLastError() != ERROR_FILE_NOT_FOUND) {
      Print(L"[diag] 无法删除 %ls（error %lu）\n", log.c_str(), ::GetLastError());
      return 4;
    }
    Print(L"[diag] 日志已清空\n");
    return 0;
  }

  WIN32_FILE_ATTRIBUTE_DATA fad{};
  const bool on = ::GetFileAttributesExW(flag.c_str(), GetFileExInfoStandard, &fad) != FALSE;
  Print(L"[diag] 开关 %ls：%ls\n", flag.c_str(), on ? L"开" : L"关");
  if (::GetFileAttributesExW(log.c_str(), GetFileExInfoStandard, &fad)) {
    const uint64_t bytes = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    Print(L"[diag] 日志 %ls：%llu 字节\n", log.c_str(),
          static_cast<unsigned long long>(bytes));
  } else {
    Print(L"[diag] 日志 %ls：不存在\n", log.c_str());
  }
  return 0;
}

}  // namespace apksx

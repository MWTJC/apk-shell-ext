#include "worker/support.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

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

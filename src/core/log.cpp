#include "core/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "core/common.h"

namespace apksx {
namespace {

// 缓存开关与日志路径：DLL 常驻 explorer，用函数内静态避免每次查询磁盘。
struct LogState {
  bool initialized = false;
  bool enabled = false;
  std::wstring path;
};

LogState& State() {
  static LogState s;
  return s;
}

void InitIfNeeded() {
  LogState& s = State();
  if (s.initialized) return;
  s.initialized = true;

  wchar_t temp[MAX_PATH * 2] = {};
  const DWORD n = ::GetTempPathW(ARRAYSIZE(temp), temp);
  if (n > 0 && n < ARRAYSIZE(temp)) {
    std::wstring flag = temp;
    flag += L"apk-shell-ext.debug.on";
    s.enabled = ::GetFileAttributesW(flag.c_str()) != INVALID_FILE_ATTRIBUTES;
  }
  if (!s.enabled) return;

  wchar_t local[MAX_PATH * 2] = {};
  const DWORD m = ::GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local));
  if (m == 0 || m >= ARRAYSIZE(local)) return;
  s.path = local;
  s.path += L"\\apk-shell-ext";
  ::CreateDirectoryW(s.path.c_str(), nullptr);
  s.path += L"\\apkthumb.log";
}

}  // namespace

bool DebugLogEnabled() {
  InitIfNeeded();
  return State().enabled;
}

void DebugLog(std::wstring_view msg) {
  if (!DebugLogEnabled()) return;

  SYSTEMTIME st{};
  ::GetLocalTime(&st);
  wchar_t line[4096];
  const int n = _snwprintf_s(line, _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu %.*s\r\n",
                             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                             st.wMilliseconds, ::GetCurrentProcessId(),
                             static_cast<int>(msg.size()), msg.data());
  if (n <= 0) return;

  // FILE_APPEND_DATA 打开，避免在 explorer 里长时间持有句柄。
  HANDLE h = ::CreateFileW(State().path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  ::WriteFile(h, line, static_cast<DWORD>(n * sizeof(wchar_t)), &written, nullptr);
  ::CloseHandle(h);
}

void DebugLogf(const wchar_t* fmt, ...) {
  if (!DebugLogEnabled()) return;
  wchar_t buf[2048];
  va_list args;
  va_start(args, fmt);
  _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
  va_end(args);
  DebugLog(buf);
}

}  // namespace apksx

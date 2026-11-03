#pragma once

#include <string>
#include <string_view>

// 诊断日志：只有 %TEMP%\apk-shell-ext.debug.on 存在时才写
// %LOCALAPPDATA%\apk-shell-ext\apkthumb.log（docs/roadmap.md M5）。
// 在 explorer 进程内不弹窗、不写事件日志，失败即静默。
namespace apksx {

bool DebugLogEnabled();
void DebugLog(std::wstring_view msg);
void DebugLogf(const wchar_t* fmt, ...);

}  // namespace apksx

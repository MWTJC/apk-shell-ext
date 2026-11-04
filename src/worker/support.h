// 运行时支持命令（M5）：状态、诊断开关、缓存清理。
//
// 安装/卸载从 M5 起交给 Inno Setup（installer\apk-shell-ext.iss）：卸载回滚是声明式的，
// 不需要我们再维护一份 journal。这里只保留"装好之后还想查/改点什么"的命令，挂在随安装
// 一起落地的 apkinfo.exe 上，所以不需要多带一个可执行文件。
#pragma once

#include <string>

namespace apksx {

// apkinfo --status：打印当前注册状态（用户级 + 整机级）与相关路径
int RunStatusCommand();

// apkinfo --diag on|off|status|clear：%TEMP%\apk-shell-ext.debug.on 开关与日志
int RunDiagCommand(const std::wstring& action);

// apkinfo --lang zh|en|auto：界面语言（写 HKCU\Software\apk-shell-ext\UiLanguage，
// 属性页跑在 explorer.exe 里，收不到环境变量，只能走注册表）
int RunLangCommand(const std::wstring& action);

// apkinfo --features [thumbs=on|off][,icons=on|off][,all=on|off]
//   不带参数 = 查询。只动"扩展名表面"那 15 个值（5×ShellEx\{E357FCCD…} + 5×DefaultIcon），
//   关闭时只删"指向我们自己"的值，别人的值原样留着并如实报告；属性页是核心功能，不可关。
//   失败（写不进去 / 试图关属性页）返回 4。
int RunFeaturesCommand(const std::wstring& action);

}  // namespace apksx

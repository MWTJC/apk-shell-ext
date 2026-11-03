// 起 `apkinfo.exe` 并取回它的 JSON：CreateProcessW + 匿名管道 + Job 对象。
//
// 三条硬约束（docs/m3-property-page.md §5）：
//   1. exe 路径只从本 DLL 所在目录取，绝不走 PATH（否则同名 exe 就是代码执行漏洞）。
//   2. 子进程挂在带 KILL_ON_JOB_CLOSE 的 Job 上：页面一关，job 句柄一关，worker 立刻消失。
//   3. 读管道有上限（4 MiB）与截止时间；取消时 TerminateJobObject，不等子进程自己退。
//
// stderr 与 stdout 合并到同一根管道：成功时 stdout 只有 JSON，失败时 stdout 为空、
// 输出全是错误文案，所以不需要两路并发读（也就不存在 stderr 写满导致的死锁）。
#pragma once

#include <windows.h>

#include <atomic>
#include <string>

namespace apksx {

enum class WorkerStatus {
  Ok = 0,
  Missing,         // 找不到 / 起不来 apkinfo.exe
  InvalidPackage,  // 退出码 2
  Timeout,         // 退出码 3，或页面侧截止时间到
  Crashed,         // 其它退出码（含 0xC0000005 之类）
  BadOutput,       // 输出超限 / 空
  Cancelled,       // 页面关闭，主动杀掉的
};

struct WorkerOutput {
  WorkerStatus status = WorkerStatus::Ok;
  DWORD exit_code = 0;
  DWORD last_error = 0;
  std::string stdout_text;  // 成功时是 JSON（UTF-8）
  std::wstring message;     // 失败时可读文案（已含 worker 的 stderr）
  std::wstring command_line;
};

// 本 DLL 同目录的 apkinfo.exe
bool FindWorkerExe(std::wstring* out);

// 按 CommandLineToArgvW 的解析规则给单个参数加引号/转义
std::wstring QuoteArg(const std::wstring& arg);

// 同步执行一次；cancel 非空时每 20 ms 轮询一次（页面关闭用）
WorkerOutput RunWorker(const std::wstring& exe, const std::wstring& file, bool want_hash,
                       int timeout_seconds, const std::atomic<bool>* cancel);

}  // namespace apksx

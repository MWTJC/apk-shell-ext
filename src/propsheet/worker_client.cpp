#include "propsheet/worker_client.h"

#include "core/i18n.h"

#include <cstdio>
#include <cwchar>
#include <string_view>

#include "core/common.h"
#include "core/log.h"
#include "propsheet/apkprop.h"

namespace apksx {
namespace {

constexpr uint64_t kMaxOutputBytes = 4ull * 1024 * 1024;
constexpr DWORD kPollMs = 20;

std::wstring JoinPathSimple(const std::wstring& dir, const wchar_t* leaf) {
  std::wstring out = dir;
  if (!out.empty() && out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
  out += leaf;
  return out;
}

std::wstring ModuleDir() {
  wchar_t path[MAX_PATH * 2] = {};
  const DWORD len = ::GetModuleFileNameW(g_module, path, ARRAYSIZE(path));
  if (len == 0 || len >= ARRAYSIZE(path)) return std::wstring();
  std::wstring full(path, len);
  const size_t slash = full.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return std::wstring();
  return full.substr(0, slash);
}

std::string TrimAscii(const std::string& text) {
  size_t begin = 0;
  while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\r' || text[begin] == '\n' ||
                                 text[begin] == '\t')) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\r' || text[end - 1] == '\n' ||
                         text[end - 1] == '\t')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

// 失败时给用户看的文案：优先用 worker 自己 stderr 的第一行
std::wstring FirstLineAsWide(std::string_view utf8) {
  const size_t newline = utf8.find('\n');
  const std::string_view first = newline == std::string_view::npos ? utf8 : utf8.substr(0, newline);
  std::wstring wide;
  if (!Utf8ToWide(TrimAscii(std::string(first)), &wide)) return std::wstring();
  return wide;
}

}  // namespace

bool FindWorkerExe(std::wstring* out) {
  if (out == nullptr) return false;
  const std::wstring dir = ModuleDir();
  if (dir.empty()) return false;
  const std::wstring candidate = JoinPathSimple(dir, L"apkinfo.exe");
  const DWORD attributes = ::GetFileAttributesW(candidate.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return false;
  }
  *out = candidate;
  return true;
}

std::wstring QuoteArg(const std::wstring& arg) {
  if (arg.empty()) return L"\"\"";
  bool needs_quotes = false;
  for (const wchar_t c : arg) {
    if (c == L' ' || c == L'\t' || c == L'"' || c == L'\n' || c == L'\v') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) return arg;

  // CommandLineToArgvW 规则：反斜杠只在引号前/结尾处才有转义含义
  std::wstring out;
  out.push_back(L'"');
  size_t backslashes = 0;
  for (const wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
      continue;
    }
    if (backslashes != 0) {
      out.append(backslashes, L'\\');
      backslashes = 0;
    }
    out.push_back(c);
  }
  if (backslashes != 0) out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

WorkerOutput RunWorker(const std::wstring& exe, const std::wstring& file, bool want_hash,
                       int timeout_seconds, const std::atomic<bool>* cancel) {
  WorkerOutput result;
  if (timeout_seconds <= 0) timeout_seconds = 8;

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!::CreatePipe(&read_pipe, &write_pipe, &sa, 64 * 1024)) {
    result.status = WorkerStatus::Missing;
    result.last_error = ::GetLastError();
    result.message = Tr(Str::PipeCreateFailed);
    return result;
  }
  ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  HANDLE nul_input = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                   OPEN_EXISTING, 0, nullptr);
  if (nul_input == INVALID_HANDLE_VALUE) nul_input = nullptr;

  std::wstring command_line = QuoteArg(exe) + L" --json --schema=1";
  if (want_hash) {
    command_line += L" --hash=sha256 --timeout=" + std::to_wstring(timeout_seconds);
  } else {
    command_line += L" --timeout=" + std::to_wstring(timeout_seconds);
  }
  command_line += L" " + QuoteArg(file);
  result.command_line = command_line;

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul_input;
  si.hStdOutput = write_pipe;
  si.hStdError = write_pipe;  // 合并：失败时 stdout 为空，错误文案就是全部输出

  PROCESS_INFORMATION pi{};
  std::wstring mutable_cmd = command_line;
  const std::wstring dir = ModuleDir();
  const BOOL created = ::CreateProcessW(exe.c_str(), mutable_cmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                        dir.empty() ? nullptr : dir.c_str(), &si, &pi);
  result.last_error = created ? 0 : ::GetLastError();

  // 父进程必须立刻放掉写端，否则读端永远等不到 EOF
  ::CloseHandle(write_pipe);
  if (nul_input != nullptr) ::CloseHandle(nul_input);

  if (!created) {
    ::CloseHandle(read_pipe);
    result.status = WorkerStatus::Missing;
    result.message = Tr(Str::WorkerStartFailed);
    DebugLogf(L"propsheet: CreateProcess failed for %s (err=%lu)", exe.c_str(),
              static_cast<unsigned long>(result.last_error));
    return result;
  }

  // Job：页面关闭 / 本进程退出时，worker 一定跟着死
  HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
  if (job != nullptr) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                   sizeof(limits)) ||
        !::AssignProcessToJobObject(job, pi.hProcess)) {
      // 失败不致命（例如 explorer 自身已在不允许 breakaway 的 job 里）：
      // 退回"取消时 TerminateProcess"的路径。
      DebugLogf(L"propsheet: job assign failed (err=%lu)", static_cast<unsigned long>(::GetLastError()));
      ::CloseHandle(job);
      job = nullptr;
    }
  }

  const ULONGLONG deadline = ::GetTickCount64() + static_cast<ULONGLONG>(timeout_seconds + 2) * 1000;
  bool cancelled = false;
  bool overflow = false;
  bool timed_out = false;
  bool process_done = false;
  std::string collected;

  while (true) {
    if (cancel != nullptr && cancel->load()) {
      cancelled = true;
      break;
    }
    // 有数据就读，没数据就等一小会儿（PeekNamedPipe 不阻塞）
    DWORD available = 0;
    if (::PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
      char buffer[16384];
      DWORD want = available < sizeof(buffer) ? available : static_cast<DWORD>(sizeof(buffer));
      DWORD got = 0;
      if (::ReadFile(read_pipe, buffer, want, &got, nullptr) && got > 0) {
        if (collected.size() + got > kMaxOutputBytes) {
          overflow = true;
          break;
        }
        collected.append(buffer, got);
        continue;
      }
    }
    if (!process_done) {
      const DWORD waited = ::WaitForSingleObject(pi.hProcess, 0);
      if (waited == WAIT_OBJECT_0) process_done = true;
    }
    if (process_done) {
      // 进程已退：把管道里剩下的读完（EOF 会失败，退出循环）
      DWORD available_end = 0;
      if (::PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available_end, nullptr) &&
          available_end > 0) {
        continue;
      }
      break;
    }
    if (::GetTickCount64() >= deadline) {
      timed_out = true;
      break;
    }
    ::Sleep(kPollMs);
  }

  if (cancelled || timed_out || overflow) {
    if (job != nullptr) {
      ::TerminateJobObject(job, 1);
    } else {
      ::TerminateProcess(pi.hProcess, 1);
    }
  }

  DWORD exit_code = 0;
  if (::WaitForSingleObject(pi.hProcess, cancelled || timed_out || overflow ? 2000 : 5000) ==
      WAIT_OBJECT_0) {
    ::GetExitCodeProcess(pi.hProcess, &exit_code);
  }
  result.exit_code = exit_code;
  result.stdout_text = collected;

  if (job != nullptr) ::CloseHandle(job);
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  ::CloseHandle(read_pipe);

  if (cancelled) {
    result.status = WorkerStatus::Cancelled;
    return result;
  }
  if (overflow) {
    result.status = WorkerStatus::BadOutput;
    result.message = Tr(Str::OutputTooBig);
    return result;
  }
  if (timed_out) {
    result.status = WorkerStatus::Timeout;
    result.message = Tr(Str::Timeout);
    return result;
  }

  switch (exit_code) {
    case 0:
      if (TrimAscii(collected).empty()) {
        result.status = WorkerStatus::BadOutput;
        result.message = Tr(Str::NoOutput);
      } else {
        result.status = WorkerStatus::Ok;
      }
      break;
    case 2: {
      result.status = WorkerStatus::InvalidPackage;
      std::wstring text = FirstLineAsWide(collected);
      std::wstring cleaned = text;
      const std::wstring prefix = L"apkinfo: ";
      if (cleaned.rfind(prefix, 0) == 0) cleaned = cleaned.substr(prefix.size());
      result.message = cleaned.empty() ? Tr(Str::NotPackage) : cleaned;
      break;
    }
    case 3:
      result.status = WorkerStatus::Timeout;
      result.message = Tr(Str::TimeoutAborted);
      break;
    default: {
      result.status = WorkerStatus::Crashed;
      wchar_t buf[96];
      _snwprintf_s(buf, _TRUNCATE, Tr(Str::WorkerCrashed),
                   static_cast<unsigned long>(exit_code));
      result.message = buf;
      break;
    }
  }

  DebugLogf(L"propsheet: worker exit=%lu status=%d bytes=%zu", static_cast<unsigned long>(exit_code),
            static_cast<int>(result.status), collected.size());
  return result;
}

}  // namespace apksx

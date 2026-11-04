// apkinfo.exe —— 全量解析 worker（M2）。契约见 docs/architecture.md §2.3。
//
//   apkinfo.exe --json [--schema=1] [--hash=sha256] [--pretty]
//               [--timeout=<seconds>] <file>
//
// 退出码：0 成功 / 2 不是有效安装包 / 3 超时 / 4 用法或内部错误
// stdout 只写 UTF-8 JSON；诊断信息走 stderr，避免污染管道。
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#include "core/apk_info.h"
#include "core/common.h"
#include "core/file_util.h"
#include "core/i18n.h"
#include "core/thumb_cache.h"
#include "worker/support.h"

namespace {

using apksx::ApkInfo;

// ------------------------------------------------------------------ 最小 JSON 写出器
class Json {
 public:
  explicit Json(bool pretty) : pretty_(pretty) { first_.push_back(true); }

  void BeginObject() { Open('{'); }
  void EndObject() { Close('}'); }
  void BeginArray() { Open('['); }
  void EndArray() { Close(']'); }

  void Key(const char* key) {
    Separate();
    out_ += '"';
    out_ += key;
    out_ += '"';
    out_ += pretty_ ? ": " : ":";
    pending_value_ = true;
  }

  void String(const std::string& value) {
    ValuePrefix();
    out_ += '"';
    EscapeInto(&out_, value);
    out_ += '"';
  }
  void Number(unsigned long long value) {
    ValuePrefix();
    out_ += std::to_string(value);
  }
  void Number(double value) {
    ValuePrefix();
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.2f", value);
    out_ += buf;
  }
  void Bool(bool value) {
    ValuePrefix();
    out_ += value ? "true" : "false";
  }

  // 便捷：key + 值
  void Str(const char* key, const std::string& value) { Key(key); String(value); }
  void Uint(const char* key, unsigned long long value) { Key(key); Number(value); }
  void Float(const char* key, double value) { Key(key); Number(value); }
  void Flag(const char* key, bool value) { Key(key); Bool(value); }
  void StrArray(const char* key, const std::vector<std::string>& items) {
    if (items.empty()) return;
    Key(key);
    BeginArray();
    for (const std::string& s : items) String(s);
    EndArray();
  }

  const std::string& str() const { return out_; }

 private:
  static void EscapeInto(std::string* out, const std::string& in) {
    for (char c : in) {
      switch (c) {
        case '"': out->append("\\\""); break;
        case '\\': out->append("\\\\"); break;
        case '\n': out->append("\\n"); break;
        case '\r': out->append("\\r"); break;
        case '\t': out->append("\\t"); break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\\u%04x",
                        static_cast<unsigned>(static_cast<unsigned char>(c)));
            out->append(buf);
          } else {
            out->push_back(c);
          }
      }
    }
  }

  void Newline() {
    if (!pretty_ || out_.empty()) return;  // 根容器的 '{' 前面不该有空行
    out_ += '\n';
    out_.append(static_cast<size_t>(first_.size() - 1) * 2, ' ');
  }

  void Separate() {
    if (pending_value_) return;      // 刚写过 key，值不需要前置逗号
    if (first_.empty()) return;
    if (first_.back()) {
      first_.back() = false;
    } else {
      out_ += ',';
    }
    Newline();
  }

  void ValuePrefix() {
    if (pending_value_) {
      pending_value_ = false;
      return;
    }
    Separate();
  }

  void Open(char c) {
    ValuePrefix();
    out_ += c;
    first_.push_back(true);
    pending_value_ = false;
  }

  void Close(char c) {
    const bool empty = first_.back();
    if (first_.size() > 1) first_.pop_back();
    if (!empty) {
      if (pretty_) {
        out_ += '\n';
        out_.append(static_cast<size_t>(first_.size() - 1) * 2, ' ');
      }
    }
    out_ += c;
    pending_value_ = false;
  }

  std::string out_;
  std::vector<bool> first_;  // 每层容器是否还没有元素
  bool pretty_ = false;
  bool pending_value_ = false;
};

unsigned long long FileTimeToUnix(uint64_t file_time) {
  const uint64_t kEpochDiff = 116444736000000000ull;  // 1601-01-01 → 1970-01-01（100ns）
  if (file_time <= kEpochDiff) return 0;
  return (file_time - kEpochDiff) / 10000000ull;
}

std::string RenderJson(const ApkInfo& info, bool pretty) {
  Json j(pretty);
  j.BeginObject();
  j.Uint("schema", 1);
  j.Str("kind", apksx::ContainerKindName(info.container));

  // ---- file
  j.Key("file");
  j.BeginObject();
  j.Str("path", info.path);
  j.Uint("size", info.size);
  j.Uint("mtime", FileTimeToUnix(info.mtime_filetime));
  if (!info.sha256.empty()) j.Str("sha256", info.sha256);
  j.EndObject();

  // ---- app
  j.Key("app");
  j.BeginObject();
  if (!info.package.empty()) j.Str("package", info.package);
  if (!info.label.empty()) j.Str("label", info.label);
  if (!info.version_name.empty()) j.Str("versionName", info.version_name);
  if (info.version_code != 0) j.Uint("versionCode", info.version_code);
  if (info.min_sdk != 0) j.Uint("minSdk", info.min_sdk);
  if (info.target_sdk != 0) j.Uint("targetSdk", info.target_sdk);
  j.StrArray("abis", info.abis);
  j.StrArray("densities", info.densities);
  j.StrArray("locales", info.locales);
  j.EndObject();

  // ---- container（M4a：只有 .xapk/.apks/.apkm 这类多 APK 容器才有）
  if (info.container != apksx::ContainerKind::kApk &&
      info.container != apksx::ContainerKind::kHap) {
    j.Key("container");
    j.BeginObject();
    j.Str("kind", apksx::ContainerKindName(info.container));
    j.Str("base", info.base_entry);
    j.Uint("apkCount", info.splits.size());
    if (!info.splits.empty()) {
      j.Key("splits");
      j.BeginArray();
      for (const apksx::SplitApk& split : info.splits) {
        j.BeginObject();
        j.Str("name", split.name);
        j.Uint("size", split.size);
        j.Str("kind", split.kind);
        if (split.is_base) j.Flag("base", true);
        j.EndObject();
      }
      j.EndArray();
    }
    j.EndObject();
  }

  // ---- hap（M4d：只有鸿蒙 .hap 才有）
  if (info.container == apksx::ContainerKind::kHap) {
    const apksx::HapInfo& hap = info.hap;
    j.Key("hap");
    j.BeginObject();
    if (!hap.module_name.empty()) j.Str("module", hap.module_name);
    if (!hap.module_type.empty()) j.Str("type", hap.module_type);
    if (!hap.main_element.empty()) j.Str("mainAbility", hap.main_element);
    if (hap.min_api != 0) j.Uint("minAPI", hap.min_api);
    if (hap.target_api != 0) j.Uint("targetAPI", hap.target_api);
    if (!hap.compile_sdk.empty()) j.Str("compileSdk", hap.compile_sdk);
    j.StrArray("deviceTypes", hap.device_types);
    j.Uint("abilities", hap.abilities);
    if (hap.extension_abilities != 0) j.Uint("extensionAbilities", hap.extension_abilities);
    if (!hap.label_ref.empty()) j.Str("labelRef", hap.label_ref);
    if (!hap.app_label_ref.empty()) j.Str("appLabelRef", hap.app_label_ref);
    j.EndObject();
  }

  // ---- icon
  if (!info.icon_source.empty()) {
    j.Key("icon");
    j.BeginObject();
    j.Str("source", info.icon_source);
    if (info.icon_width != 0) {
      j.Uint("width", info.icon_width);
      j.Uint("height", info.icon_height);
    }
    j.Flag("adaptive", info.icon_adaptive);
    if (info.icon_from_container) j.Flag("fromContainer", true);
    j.EndObject();
  }

  // ---- components
  j.Key("components");
  j.BeginObject();
  j.Uint("activities", info.components.activities);
  j.Uint("services", info.components.services);
  j.Uint("receivers", info.components.receivers);
  j.Uint("providers", info.components.providers);
  j.EndObject();

  // ---- permissions
  if (!info.permissions.empty()) {
    j.Key("permissions");
    j.BeginArray();
    for (const std::string& p : info.permissions) {
      j.BeginObject();
      j.Str("name", p);
      j.Flag("declared", true);
      j.EndObject();
    }
    j.EndArray();
  }

  // ---- features
  if (!info.features.empty()) {
    j.Key("features");
    j.BeginArray();
    for (const apksx::ApkFeature& f : info.features) {
      j.BeginObject();
      j.Str("name", f.name);
      j.Flag("required", f.required);
      j.EndObject();
    }
    j.EndArray();
  }

  // ---- signature
  // 未签名 / 结构不认识的包整体省略该字段（JSON 里"没有"比 null 更好判断）
  if (!info.signature.empty()) {
    j.Key("signature");
    j.BeginObject();
    j.StrArray("scheme", info.signature.schemes);
    j.Key("certs");
    j.BeginArray();
    for (const apksx::SignatureCert& cert : info.signature.certs) {
      j.BeginObject();
      j.Str("subject", cert.subject);
      j.Str("sha256", cert.sha256);
      if (!cert.not_before.empty()) j.Str("notBefore", cert.not_before);
      if (!cert.not_after.empty()) j.Str("notAfter", cert.not_after);
      j.EndObject();
    }
    j.EndArray();
    j.EndObject();
  }

  // ---- diagnostics
  j.Key("diagnostics");
  j.BeginObject();
  j.StrArray("warnings", info.warnings);
  j.Float("elapsedMs", info.elapsed_ms);
  j.EndObject();

  j.EndObject();
  return j.str();
}

std::string RenderText(const ApkInfo& info) {
  std::string out;
  auto line = [&out](const char* key, const std::string& value) {
    if (value.empty()) return;
    out += key;
    out += ": ";
    out += value;
    out += '\n';
  };
  line("path", info.path);
  if (info.container == apksx::ContainerKind::kHap) {
    line("hap", info.hap.module_name + " (" + info.hap.module_type + ") / minAPI " +
                   std::to_string(info.hap.min_api) + " / targetAPI " +
                   std::to_string(info.hap.target_api) + " / " +
                   std::to_string(info.hap.abilities) + " abilities");
  }
  if (info.container != apksx::ContainerKind::kApk &&
      info.container != apksx::ContainerKind::kHap) {
    line("container", std::string(apksx::ContainerKindName(info.container)) + " / base " +
                          info.base_entry + " / " + std::to_string(info.splits.size()) + " apks");
  }
  line("package", info.package);
  line("label", info.label);
  line("version", info.version_name + " (" + std::to_string(info.version_code) + ")");
  line("sdk", "min " + std::to_string(info.min_sdk) + " / target " +
                  std::to_string(info.target_sdk));
  if (!info.abis.empty()) {
    std::string abis;
    for (const std::string& a : info.abis) abis += (abis.empty() ? "" : ", ") + a;
    line("abis", abis);
  }
  if (!info.densities.empty()) {
    std::string v;
    for (const std::string& d : info.densities) v += (v.empty() ? "" : ", ") + d;
    line("densities", v);
  }
  line("icon", info.icon_source);
  line("components", "activities " + std::to_string(info.components.activities) + ", services " +
                         std::to_string(info.components.services) + ", receivers " +
                         std::to_string(info.components.receivers) + ", providers " +
                         std::to_string(info.components.providers));
  line("permissions", std::to_string(info.permissions.size()));
  if (!info.features.empty()) {
    size_t required = 0;
    for (const apksx::ApkFeature& f : info.features) {
      if (f.required) ++required;
    }
    line("features", std::to_string(info.features.size()) + " (" +
                         std::to_string(required) + " required)");
  }
  if (!info.signature.schemes.empty()) {
    std::string schemes;
    for (const std::string& s : info.signature.schemes) {
      schemes += (schemes.empty() ? "" : ", ") + s;
    }
    line("signature", schemes);
  }
  for (const apksx::SignatureCert& cert : info.signature.certs) {
    line("cert", cert.sha256 + "  " + cert.subject);
  }
  line("sha256", info.sha256);
  for (const std::string& w : info.warnings) line("warning", w);
  char ms[32];
  _snprintf_s(ms, sizeof(ms), _TRUNCATE, "%.1f ms", info.elapsed_ms);
  line("elapsed", ms);
  return out;
}

struct Options {
  std::wstring file;
  bool json = false;
  bool hash = false;
  bool pretty = false;
  int timeout_seconds = 8;
};

struct WatchdogArg {
  DWORD ms;
};

DWORD WINAPI WatchdogProc(LPVOID param) {
  auto* arg = static_cast<WatchdogArg*>(param);
  ::Sleep(arg->ms);
  std::fwprintf(stderr, L"apkinfo: timeout after %lu ms\n", arg->ms);
  ::ExitProcess(3);
}

// stderr 用 UTF-8，避免控制台代码页把中文诊断变成问号；读取方按字节转码即可。
void WriteStderr(const std::string& utf8) {
  std::fwrite(utf8.data(), 1, utf8.size(), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

void PrintUsage() {
  std::fwprintf(stderr,
                L"usage: apkinfo --json [--schema=1] [--hash=sha256] [--pretty]\n"
                L"               [--timeout=<seconds>] <file>\n"
                L"       apkinfo --clear-cache [--cache-info]\n"
                L"       apkinfo --status | --diag on|off|status|clear\n");
}

// 缓存清理（M5）：缩略图缓存是自建的，卸载后也要能一键清空，所以放在会随安装一起装上的 worker 里。
// 规则与粗粒度一致：只动 %LOCALAPPDATA%\apk-shell-ext\thumbs 下的文件（ThumbCache::ClearAll）。
int DoCacheCommand(bool clear, bool info) {
  uint64_t removed = 0;
  std::wstring dir = apksx::ThumbCache::RootDir();
  if (clear) {
    if (!apksx::ThumbCache::ClearAll(&removed)) {
      std::fwprintf(stderr, L"apkinfo: cache directory unavailable\n");
      return 4;
    }
    std::printf("cleared %llu bytes\n", static_cast<unsigned long long>(removed));
  }
  if (info) {
    uint64_t total = 0;
    uint32_t files = 0;
    if (!dir.empty()) {
      WIN32_FIND_DATAW fd{};
      HANDLE h = ::FindFirstFileW(apksx::JoinPath(dir, L"*").c_str(), &fd);
      if (h != INVALID_HANDLE_VALUE) {
        do {
          if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
          total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
          ++files;
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
      }
    }
    std::printf("%u files, %llu bytes in ", files, static_cast<unsigned long long>(total));
    std::printf("%s\n", dir.empty() ? "(no cache dir)" : "");
    if (!dir.empty()) {
      std::string utf8;
      if (apksx::WideToUtf8(dir, &utf8)) std::printf("%s\n", utf8.c_str());
    }
  }
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  (void)com;

  Options opt;
  bool clear_cache = false;
  bool cache_info = false;
  std::wstring diag_action;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--json") {
      opt.json = true;
    } else if (arg == L"--pretty") {
      opt.pretty = true;
    } else if (arg == L"--hash=sha256") {
      opt.hash = true;
    } else if (arg == L"--schema=1") {
      // 目前只有 v1
    } else if (arg.rfind(L"--timeout=", 0) == 0) {
      opt.timeout_seconds = _wtoi(arg.c_str() + 10);
      if (opt.timeout_seconds <= 0) opt.timeout_seconds = 8;
    } else if (arg == L"--clear-cache") {
      clear_cache = true;
    } else if (arg == L"--cache-info") {
      cache_info = true;
    } else if (arg == L"--status") {
      ::CoUninitialize();
      return apksx::RunStatusCommand();
    } else if (arg.rfind(L"--diag=", 0) == 0) {
      ::CoUninitialize();
      return apksx::RunDiagCommand(arg.substr(7));
    } else if (arg == L"--diag") {
      diag_action = (i + 1 < argc) ? argv[++i] : L"status";
    } else if (arg.rfind(L"--lang=", 0) == 0) {
      ::CoUninitialize();
      return apksx::RunLangCommand(arg.substr(7));
    } else if (arg == L"--lang") {
      ::CoUninitialize();
      return apksx::RunLangCommand((i + 1 < argc) ? argv[++i] : L"auto");
    } else if (!arg.empty() && arg[0] == L'-') {
      PrintUsage();
      return 4;
    } else {
      opt.file = arg;
    }
  }
  if (!diag_action.empty()) {
    ::CoUninitialize();
    return apksx::RunDiagCommand(diag_action);
  }
  if (clear_cache || cache_info) {
    const int code = DoCacheCommand(clear_cache, cache_info);
    ::CoUninitialize();
    return code;
  }
  if (opt.file.empty()) {
    PrintUsage();
    return 4;
  }

  // 解析挂死时 8 s 自杀（docs/architecture.md §2.3）
  WatchdogArg wd{static_cast<DWORD>(opt.timeout_seconds) * 1000u};
  HANDLE watchdog = ::CreateThread(nullptr, 0, WatchdogProc, &wd, 0, nullptr);

  // 测试钩子：放在 watchdog 之后，才能验证超时路径（正常使用不设置该环境变量）
  {
    wchar_t delay[32] = {};
    if (::GetEnvironmentVariableW(L"APKSX_TEST_DELAY_MS", delay, ARRAYSIZE(delay)) > 0) {
      ::Sleep(static_cast<DWORD>(_wtoi(delay)));
    }
  }

  // 测试钩子：属性页要验证"解析器崩溃"的文案（M3 验收）。环境变量管不到 explorer 拉起的子进程，
  // 所以用标志文件：%TEMP%\apk-shell-ext.test-abort.on 存在时，等 300 ms 后模拟访问违例。
  {
    wchar_t temp[MAX_PATH * 2] = {};
    const DWORD n = ::GetTempPathW(ARRAYSIZE(temp), temp);
    if (n > 0 && n < ARRAYSIZE(temp)) {
      std::wstring flag = temp;
      flag += L"apk-shell-ext.test-abort.on";
      if (::GetFileAttributesW(flag.c_str()) != INVALID_FILE_ATTRIBUTES) {
        ::Sleep(300);
        // 直接用异常退出码退出（RaiseException 会把进程交给 WER，页面只能看到"超时"）
        ::ExitProcess(0xC0000005u);
      }
    }
  }

  apksx::ApkInfo info;
  apksx::Error err;
  const bool ok = apksx::CollectApkInfo(opt.file, opt.hash, &info, &err);

  if (watchdog != nullptr) {
    ::TerminateThread(watchdog, 0);
    ::CloseHandle(watchdog);
  }

  if (!ok) {
    std::string message;
    const std::wstring text =
        apksx::TrMsg(err.message.empty() ? L"解析失败" : err.message);
    if (!apksx::WideToUtf8(text, &message)) {
      message = "parse failed";
    }
    WriteStderr(std::string("apkinfo: ") + message);
    ::CoUninitialize();
    return 2;
  }

  const std::string text = opt.json ? RenderJson(info, opt.pretty) : RenderText(info);
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);

  ::CoUninitialize();
  return 0;
}

// thumbtest.exe —— M1 的验收工具（docs/verification.md §2）：直接驱动 provider，
// 输出冷/热耗时、中心像素颜色、PNG 样本，并对坏包做批量回归。
//
//   thumbtest --probe  <apk>                 只解析清单与图标条目，不解码
//   thumbtest --file   <apk> [--out a.png]   单文件：耗时 + 颜色（可选存图）
//   thumbtest --dir    <dir> [--repeat 3]    目录批量：P50/P95
//   thumbtest --list   <paths.txt>           批量（每行一个路径），失败继续
//   thumbtest --shell  --file <apk>          走 IShellItemImageFactory（验证注册是否生效）
//   thumbtest --clean-cache                  清空自建缓存
// 公共参数：--size 256 --json --dll <apkthumb.dll>
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/apk_icon.h"
#include "core/common.h"
#include "core/com_ptr.h"
#include "core/file_util.h"
#include "core/hap.h"
#include "core/package.h"
#include "core/thumb_cache.h"
#include "core/wic_image.h"
#include "core/zip_reader.h"

namespace {

// apkthumb.dll 的 CLSID（与 src/thumbs/apkthumb.h 保持一致；此处不链接 DLL 的符号）
const CLSID kThumbClsid = {0xA7C4E1B0,
                           0x5E8D,
                           0x4C3A,
                           {0x9F, 0x2B, 0x6D, 0x1C, 0x8E, 0x4A, 0x7B, 0x01}};

using apksx::ComPtr;

double QpcToMs(long long ticks) {
  LARGE_INTEGER f{};
  ::QueryPerformanceFrequency(&f);
  return static_cast<double>(ticks) * 1000.0 / static_cast<double>(f.QuadPart);
}

long long QpcNow() {
  LARGE_INTEGER t{};
  ::QueryPerformanceCounter(&t);
  return t.QuadPart;
}

std::wstring ExeDir() {
  wchar_t buf[MAX_PATH * 2] = {};
  ::GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
  std::wstring path = buf;
  const size_t pos = path.find_last_of(L"\\/");
  return pos == std::wstring::npos ? path : path.substr(0, pos);
}

struct CenterColor {
  bool ok = false;
  uint8_t b = 0;
  uint8_t g = 0;
  uint8_t r = 0;
  uint8_t a = 0;
};

CenterColor SampleCenter(HBITMAP bmp) {
  CenterColor out;
  DIBSECTION ds{};
  if (::GetObjectW(bmp, sizeof(ds), &ds) != sizeof(ds)) return out;
  if (ds.dsBm.bmBits == nullptr || ds.dsBm.bmBitsPixel != 32) return out;
  const int w = ds.dsBm.bmWidth;
  const int h = ds.dsBm.bmHeight < 0 ? -ds.dsBm.bmHeight : ds.dsBm.bmHeight;
  if (w <= 0 || h <= 0) return out;
  const auto* base = static_cast<const uint8_t*>(ds.dsBm.bmBits);
  const size_t stride = static_cast<size_t>(w) * 4u;
  const uint8_t* px = base + static_cast<size_t>(h / 2) * stride + static_cast<size_t>(w / 2) * 4u;
  out.b = px[0];
  out.g = px[1];
  out.r = px[2];
  out.a = px[3];
  out.ok = true;
  return out;
}

// HBITMAP → PNG（测试工具里用 WIC，与 DLL 的解码路径无关）
bool SavePng(HBITMAP bmp, const std::wstring& path) {
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.Put())))) {
    return false;
  }
  ComPtr<IWICBitmap> wic_bmp;
  if (FAILED(factory->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapUsePremultipliedAlpha,
                                              wic_bmp.Put()))) {
    return false;
  }
  ComPtr<IStream> stream;
  HGLOBAL global = nullptr;
  if (FAILED(::CreateStreamOnHGlobal(nullptr, TRUE, reinterpret_cast<IStream**>(stream.Put())))) {
    return false;
  }
  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.Put()))) return false;
  if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
  ComPtr<IWICBitmapFrameEncode> frame;
  if (FAILED(encoder->CreateNewFrame(frame.Put(), nullptr))) return false;
  if (FAILED(frame->Initialize(nullptr))) return false;
  UINT w = 0;
  UINT h = 0;
  wic_bmp->GetSize(&w, &h);
  frame->SetSize(w, h);
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppPBGRA;
  frame->SetPixelFormat(&fmt);
  if (FAILED(frame->WriteSource(wic_bmp.Get(), nullptr))) return false;
  if (FAILED(frame->Commit())) return false;
  if (FAILED(encoder->Commit())) return false;
  if (FAILED(::GetHGlobalFromStream(stream.Get(), &global))) return false;

  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  const SIZE_T size = ::GlobalSize(global);
  const void* data = ::GlobalLock(global);
  bool ok = false;
  if (data != nullptr) {
    DWORD written = 0;
    ok = ::WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) != FALSE &&
         written == size;
    ::GlobalUnlock(global);
  }
  ::CloseHandle(file);
  return ok;
}

// ---------------------------------------------------------------- provider 驱动
struct ProviderHost {
  HMODULE dll = nullptr;
  ComPtr<IClassFactory> factory;

  bool Load(const std::wstring& dll_path) {
    dll = ::LoadLibraryW(dll_path.c_str());
    if (dll == nullptr) return false;
    using Fn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto fn = reinterpret_cast<Fn>(::GetProcAddress(dll, "DllGetClassObject"));
    if (fn == nullptr) return false;
    return SUCCEEDED(fn(kThumbClsid, IID_PPV_ARGS(factory.Put())));
  }

  bool Thumbnail(const std::wstring& file, UINT cx, HBITMAP* out, double* ms) {
    ComPtr<IInitializeWithFile> init;
    if (!factory || FAILED(factory->CreateInstance(nullptr, IID_PPV_ARGS(init.Put())))) {
      return false;
    }
    if (FAILED(init->Initialize(file.c_str(), STGM_READ))) return false;
    ComPtr<IThumbnailProvider> provider;
    if (FAILED(init.As(&provider))) return false;

    const long long t0 = QpcNow();
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
    HBITMAP bmp = nullptr;
    const HRESULT hr = provider->GetThumbnail(cx, &bmp, &alpha);
    if (ms != nullptr) *ms = QpcToMs(QpcNow() - t0);
    if (FAILED(hr) || bmp == nullptr) return false;
    *out = bmp;
    return true;
  }

  bool ShellThumbnail(const std::wstring& file, UINT cx, HBITMAP* out, double* ms) {
    // SHCreateItemFromParsingName 只认反斜杠路径（正斜杠会 E_INVALIDARG）
    std::wstring native = file;
    for (wchar_t& c : native) {
      if (c == L'/') c = L'\\';
    }
    ComPtr<IShellItemImageFactory> isif;
    last_hr_ = ::SHCreateItemFromParsingName(native.c_str(), nullptr,
                                              IID_PPV_ARGS(isif.Put()));
    create_hr_ = last_hr_;
    if (FAILED(last_hr_)) return false;
    SIZE size{static_cast<LONG>(cx), static_cast<LONG>(cx)};
    const long long t0 = QpcNow();
    HBITMAP bmp = nullptr;
    last_hr_ = isif->GetImage(size, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &bmp);
    if (ms != nullptr) *ms = QpcToMs(QpcNow() - t0);
    if (FAILED(last_hr_) || bmp == nullptr) return false;
    *out = bmp;
    return true;
  }

  HRESULT last_hr() const { return last_hr_; }
  HRESULT create_hr() const { return create_hr_; }

 private:
  HRESULT last_hr_ = S_OK;
  HRESULT create_hr_ = S_OK;
};

struct Stats {
  std::vector<double> values;
  void Add(double v) { values.push_back(v); }
  double Percentile(double p) const {
    if (values.empty()) return 0.0;
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5);
    if (idx >= sorted.size()) idx = sorted.size() - 1;
    return sorted[idx];
  }
  double Max() const {
    return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
  }
};

std::string JsonEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\\u%04x",
                      static_cast<unsigned>(static_cast<unsigned char>(c)));
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

void PrintJsonString(const char* key, const std::wstring& value, bool comma = true) {
  std::string utf8;
  apksx::WideToUtf8(value, &utf8);
  std::printf("\"%s\":\"%s\"%s", key, JsonEscape(utf8).c_str(), comma ? "," : "");
}

void PrintColor(const CenterColor& c) {
  if (!c.ok) {
    std::printf("\"color\":null,");
    return;
  }
  std::printf("\"color\":\"%02X%02X%02X%02X\",", c.r, c.g, c.b, c.a);
}

// ---------------------------------------------------------------- 子命令
int Probe(const std::wstring& file, bool json) {
  apksx::Deadline deadline(0);
  apksx::ManifestInfo info;
  apksx::Error err;
  apksx::IconLocation icon;

  // 打开文件 → 判定 am I HAP / 多 APK 容器 / 普通 APK（与 provider 同一套代码）
  apksx::PackageOpen package;
  apksx::HapInfo hap;
  apksx::ZipReader outer;
  bool zip_ok = false;
  if (auto source = apksx::OpenFileSource(file, &err)) {
    zip_ok = outer.OpenSource(source, &err);
  }
  const bool is_hap = zip_ok && apksx::LooksLikeHap(outer);
  apksx::ZipReader base;
  bool package_ok = zip_ok;
  if (zip_ok && !is_hap) {
    package_ok = apksx::ResolvePackage(outer, &package, &err);
  }

  bool manifest_ok = false;
  bool icon_ok = false;
  const apksx::ZipReader* apk_zip = &outer;

  if (is_hap) {
    manifest_ok = apksx::ReadHapManifest(outer, &hap, &err);
    if (manifest_ok) {
      info.package = hap.bundle_name;
      info.label = hap.label;
      info.version_name = hap.version_name;
      info.version_code = hap.version_code;
      info.min_sdk = hap.min_api;
      info.target_sdk = hap.target_api;
      info.has_icon = true;
      apksx::HapIcon hap_icon;
      apksx::Error icon_error;
      if (apksx::ReadHapIcon(outer, hap, &hap_icon, &icon_error)) {
        icon.entry_path = hap_icon.path;
        icon.note = hap_icon.note;
        icon_ok = true;
      }
    }
  } else if (package_ok && package.kind != apksx::ContainerKind::kApk) {
    if (base.OpenSource(package.apk, &err)) apk_zip = &base;
  }

  if (!is_hap) {
    manifest_ok = apk_zip->IsOpen() && apksx::ReadManifestFromZip(*apk_zip, &info, &err, &deadline);
    icon_ok = manifest_ok && apksx::LocateIconInZip(*apk_zip, info, &icon, &err, &deadline);
    // base 里没有图标时，真正会被画的是容器根上的 icon.png（与 provider 的行为一致）
    if (package_ok && package.kind != apksx::ContainerKind::kApk && icon.entry_path.empty() &&
        !package.root_icon.empty()) {
      icon.entry_path = package.root_icon;
      icon.note = "容器根上的 icon.png（base 里没有可用图标）";
      icon_ok = true;
    }
  }

  if (json) {
    std::printf("{");
    PrintJsonString("file", file);
    std::printf("\"container\":\"%s\",", is_hap ? "hap" : apksx::ContainerKindName(package.kind));
    if (!package.base_entry.empty()) {
      std::printf("\"base\":\"%s\",", JsonEscape(package.base_entry).c_str());
    }
    std::printf("\"manifestOk\":%s,", manifest_ok ? "true" : "false");
    std::printf("\"iconOk\":%s,", icon_ok ? "true" : "false");
    std::string pkg, label, ver, note;
    pkg = info.package;
    label = info.label;
    ver = info.version_name;
    note = icon.note;
    std::printf("\"package\":\"%s\",", JsonEscape(pkg).c_str());
    std::printf("\"label\":\"%s\",", JsonEscape(label).c_str());
    std::printf("\"versionName\":\"%s\",", JsonEscape(ver).c_str());
    std::printf("\"versionCode\":%u,", info.version_code);
    std::printf("\"minSdk\":%u,", info.min_sdk);
    std::printf("\"targetSdk\":%u,", info.target_sdk);
    std::printf("\"hasIcon\":%s,", info.has_icon ? "true" : "false");
    std::printf("\"iconResId\":%u,", info.icon_res_id);
    std::printf("\"iconEntry\":\"%s\",", JsonEscape(icon.entry_path).c_str());
    std::printf("\"density\":%u,", icon.density);
    std::printf("\"note\":\"%s\",", JsonEscape(note).c_str());
    std::printf("\"elapsedMs\":%.2f", deadline.elapsed_ms());
    std::printf("}\n");
  } else {
    std::wprintf(L"%ls\n  清单: %ls\n", file.c_str(), manifest_ok ? L"ok" : err.message.c_str());
    if (is_hap) {
      std::wprintf(L"  container: hap  module=%hs  abilities=%u  minAPI=%u\n",
                   hap.module_name.c_str(), hap.abilities, hap.min_api);
    } else if (package_ok && package.kind != apksx::ContainerKind::kApk) {
      std::wprintf(L"  容器: %hs  base=%hs  %zu 个 APK\n",
                   apksx::ContainerKindName(package.kind), package.base_entry.c_str(),
                   package.splits.size());
    }
    std::wprintf(L"  package=%hs versionName=%hs versionCode=%u minSdk=%u targetSdk=%u\n",
                 info.package.c_str(), info.version_name.c_str(), info.version_code, info.min_sdk,
                 info.target_sdk);
    std::wprintf(L"  label=%hs hasIcon=%d iconResId=0x%08X\n", info.label.c_str(),
                 info.has_icon ? 1 : 0, info.icon_res_id);
    std::wprintf(L"  图标: %hs (density=%u note=%hs heuristic=%d)\n", icon.entry_path.c_str(),
                 icon.density, icon.note.c_str(), icon.heuristic ? 1 : 0);
    if (!icon_ok) std::wprintf(L"  错误: %ls\n", err.message.c_str());
  }
  return (manifest_ok && icon_ok) ? 0 : 1;
}

int OneFile(ProviderHost& host, const std::wstring& file, UINT cx, const std::wstring& out_png,
            bool use_shell, bool json) {
  HBITMAP bmp = nullptr;
  double ms = 0.0;
  const bool ok = use_shell ? host.ShellThumbnail(file, cx, &bmp, &ms)
                            : host.Thumbnail(file, cx, &bmp, &ms);
  if (!ok) {
    if (json) {
      std::printf("{");
      PrintJsonString("file", file);
      std::printf("\"ok\":false,\"ms\":%.2f,\"hr\":\"0x%08lX\",\"createHr\":\"0x%08lX\"}\n", ms,
                  static_cast<unsigned long>(host.last_hr()),
                  static_cast<unsigned long>(host.create_hr()));
    } else {
      std::wprintf(L"FAIL %ls (%.2f ms, hr=0x%08lX)\n", file.c_str(), ms,
                   static_cast<unsigned long>(host.last_hr()));
    }
    return 1;
  }

  const CenterColor color = SampleCenter(bmp);
  BITMAP bm{};
  ::GetObjectW(bmp, sizeof(bm), &bm);
  if (!out_png.empty()) SavePng(bmp, out_png);

  if (json) {
    std::printf("{");
    PrintJsonString("file", file);
    std::printf("\"ok\":true,\"ms\":%.2f,", ms);
    std::printf("\"w\":%ld,\"h\":%ld,", bm.bmWidth, bm.bmHeight);
    PrintColor(color);
    PrintJsonString("png", out_png, false);
    std::printf("}\n");
  } else {
    std::wprintf(L"OK   %ls  %.2f ms  %ldx%ld  rgba=%02X%02X%02X%02X\n", file.c_str(), ms,
                 bm.bmWidth, bm.bmHeight, color.r, color.g, color.b, color.a);
  }
  ::DeleteObject(bmp);
  return 0;
}

int Batch(ProviderHost& host, const std::vector<std::wstring>& files, UINT cx, int repeat,
          bool use_shell, bool json) {
  Stats cold;
  Stats hot;
  int failed = 0;

  for (const std::wstring& file : files) {
    std::vector<double> times;
    bool ok = false;
    for (int i = 0; i < repeat; ++i) {
      HBITMAP bmp = nullptr;
      double ms = 0.0;
      ok = use_shell ? host.ShellThumbnail(file, cx, &bmp, &ms) : host.Thumbnail(file, cx, &bmp, &ms);
      if (bmp != nullptr) ::DeleteObject(bmp);
      times.push_back(ms);
      if (!ok) break;
    }
    if (times.empty()) continue;
    if (ok) {
      cold.Add(times.front());
      if (times.size() > 1) hot.Add(times.back());
    } else {
      ++failed;
    }
    if (json) {
      std::printf("{");
      PrintJsonString("file", file);
      std::printf("\"ok\":%s,\"first\":%.2f,\"last\":%.2f}\n", ok ? "true" : "false", times.front(),
                  times.back());
    } else {
      std::wprintf(L"%ls %ls first=%.2f ms last=%.2f ms\n", ok ? L"OK  " : L"FAIL", file.c_str(),
                   times.front(), times.back());
    }
  }

  const int total = static_cast<int>(files.size());
  std::printf(
      "\nsummary{\"count\":%d,\"failed\":%d,\"coldP50\":%.2f,\"coldP95\":%.2f,\"coldMax\":%.2f,"
      "\"hotP50\":%.2f,\"hotP95\":%.2f,\"hotMax\":%.2f}\n",
      total, failed, cold.Percentile(0.5), cold.Percentile(0.95), cold.Max(), hot.Percentile(0.5),
      hot.Percentile(0.95), hot.Max());
  return failed == 0 ? 0 : 2;
}

std::vector<std::wstring> CollectFiles(const std::wstring& dir, const wchar_t* pattern) {
  std::vector<std::wstring> out;
  WIN32_FIND_DATAW fd{};
  HANDLE h = ::FindFirstFileW(apksx::JoinPath(dir, pattern).c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    out.push_back(apksx::JoinPath(dir, fd.cFileName));
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::wstring file, dir, list, dll_path, out_png;
  UINT cx = 256;
  int repeat = 3;
  bool json = false;
  bool use_shell = false;
  bool probe = false;
  bool clean = false;

  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    auto next = [&](std::wstring* target) {
      if (i + 1 < argc) *target = argv[++i];
    };
    if (arg == L"--file") {
      next(&file);
    } else if (arg == L"--dir") {
      next(&dir);
    } else if (arg == L"--list") {
      next(&list);
    } else if (arg == L"--out") {
      next(&out_png);
    } else if (arg == L"--dll") {
      next(&dll_path);
    } else if (arg == L"--size") {
      std::wstring v;
      next(&v);
      cx = static_cast<UINT>(_wtoi(v.c_str()));
    } else if (arg == L"--repeat") {
      std::wstring v;
      next(&v);
      repeat = _wtoi(v.c_str());
    } else if (arg == L"--json") {
      json = true;
    } else if (arg == L"--shell") {
      use_shell = true;
    } else if (arg == L"--probe") {
      probe = true;
    } else if (arg == L"--clean-cache") {
      clean = true;
    } else if (!arg.empty() && arg[0] != L'-') {
      file = arg;  // 容错：允许直接把路径当参数
    }
  }
  if (cx < 16) cx = 256;
  if (repeat < 1) repeat = 1;

  // 相对路径在 --shell 模式下会被 SHCreateItemFromParsingName 当非法路径（0x80070002），
  // 统一转成绝对路径再用。
  auto absolutize = [](std::wstring* p) {
    if (p->empty() || std::filesystem::path(*p).is_absolute()) return;
    std::error_code ec;
    const auto abs = std::filesystem::absolute(*p, ec);
    if (!ec) *p = abs.wstring();
  };
  absolutize(&file);
  absolutize(&dir);
  absolutize(&list);

  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  (void)com;

  if (clean) {
    uint64_t removed = 0;
    apksx::ThumbCache::ClearAll(&removed);
    std::wprintf(L"cleared %llu bytes from %ls\n", static_cast<unsigned long long>(removed),
                 apksx::ThumbCache::RootDir().c_str());
    ::CoUninitialize();
    return 0;
  }

  if (probe && !file.empty()) {
    const int rc = Probe(file, json);
    ::CoUninitialize();
    return rc;
  }

  ProviderHost host;
  if (!use_shell) {
    const std::wstring path = dll_path.empty() ? apksx::JoinPath(ExeDir(), L"apkthumb.dll")
                                               : dll_path;
    if (!host.Load(path)) {
      std::fwprintf(stderr, L"failed to load provider dll: %ls\n", path.c_str());
      ::CoUninitialize();
      return 3;
    }
  }

  int rc = 0;
  if (!file.empty()) {
    rc = OneFile(host, file, cx, out_png, use_shell, json);
  } else if (!dir.empty()) {
    const std::vector<std::wstring> files = CollectFiles(dir, L"*.apk");
    rc = Batch(host, files, cx, repeat, use_shell, json);
  } else if (!list.empty()) {
    std::ifstream in(list);
    std::vector<std::wstring> files;
    std::string line;
    while (std::getline(in, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
      if (line.empty()) continue;
      std::wstring wide;
      if (apksx::Utf8ToWide(line, &wide)) files.push_back(wide);
    }
    rc = Batch(host, files, cx, repeat, use_shell, json);
  } else {
    std::fwprintf(stderr,
                  L"usage: thumbtest --probe|--file|--dir|--list|--clean-cache [--size N] "
                  L"[--repeat N] [--json] [--shell] [--dll path] [--out png]\n");
    rc = 1;
  }

  ::CoUninitialize();
  return rc;
}

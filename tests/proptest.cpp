// 属性页的"不走资源管理器"的验收工具：
//   显式加载 apkprop.dll → DllGetClassObject → IShellPropSheetExt::AddPages → PropertySheetW
// 这样能分别回答三个问题：DLL 能不能加载 / 类厂与页面能不能建 / 页面画成什么样。
//
//   proptest --file <apk> [--dll <apkprop.dll>] [--auto-close <ms>] [--verbose]
//
// 退出码：0 成功打开（或被 --auto-close 关掉）/ 2 用法错 / 3 加载或建页失败
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <wincodec.h>

#include <cstdio>
#include <string>
#include <vector>

#include "core/com_ptr.h"

namespace {

const CLSID kPropClsid = {0xA7C4E1B0, 0x5E8D, 0x4C3A, {0x9F, 0x2B, 0x6D, 0x1C, 0x8E, 0x4A, 0x7B, 0x02}};

struct PageCollector {
  std::vector<HPROPSHEETPAGE> pages;
};

int CALLBACK AddPageProc(HPROPSHEETPAGE page, LPARAM lparam) {
  auto* collector = reinterpret_cast<PageCollector*>(lparam);
  collector->pages.push_back(page);
  return TRUE;
}

std::wstring ExeDir() {
  wchar_t path[MAX_PATH * 2] = {};
  const DWORD len = ::GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
  std::wstring full(path, len);
  const size_t slash = full.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : full.substr(0, slash);
}

bool SavePng(HBITMAP bitmap, const std::wstring& path) {
  apksx::ComPtr<IWICImagingFactory> factory;
  if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.Put())))) {
    return false;
  }
  apksx::ComPtr<IWICBitmap> wic_bitmap;
  if (FAILED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUsePremultipliedAlpha,
                                              wic_bitmap.Put()))) {
    return false;
  }
  apksx::ComPtr<IStream> stream;
  if (FAILED(::CreateStreamOnHGlobal(nullptr, TRUE, reinterpret_cast<IStream**>(stream.Put())))) {
    return false;
  }
  apksx::ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.Put()))) return false;
  if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
  apksx::ComPtr<IWICBitmapFrameEncode> frame;
  if (FAILED(encoder->CreateNewFrame(frame.Put(), nullptr))) return false;
  if (FAILED(frame->Initialize(nullptr))) return false;
  UINT width = 0;
  UINT height = 0;
  wic_bitmap->GetSize(&width, &height);
  frame->SetSize(width, height);
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
  frame->SetPixelFormat(&format);
  if (FAILED(frame->WriteSource(wic_bitmap.Get(), nullptr))) return false;
  if (FAILED(frame->Commit())) return false;
  if (FAILED(encoder->Commit())) return false;
  STATSTG stat{};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return false;
  LARGE_INTEGER zero{};
  if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;
  std::vector<uint8_t> bytes(static_cast<size_t>(stat.cbSize.QuadPart));
  ULONG read = 0;
  if (FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read))) return false;
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) return false;
  fwrite(bytes.data(), 1, read, file);
  fclose(file);
  return true;
}

// 把属性表窗口画进一张 HBITMAP（PW_RENDERFULLCONTENT 对 classic 对话框有效）
bool CaptureWindow(HWND hwnd, HBITMAP* out) {
  RECT rect{};
  if (!::GetWindowRect(hwnd, &rect)) return false;
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) return false;
  HDC screen = ::GetDC(nullptr);
  HDC memory = ::CreateCompatibleDC(screen);
  HBITMAP bitmap = ::CreateCompatibleBitmap(screen, width, height);
  HGDIOBJ old = ::SelectObject(memory, bitmap);
  const BOOL ok = ::PrintWindow(hwnd, memory, 0x00000002 /* PW_RENDERFULLCONTENT */);
  ::SelectObject(memory, old);
  ::DeleteDC(memory);
  ::ReleaseDC(nullptr, screen);
  if (!ok) {
    ::DeleteObject(bitmap);
    return false;
  }
  *out = bitmap;
  return true;
}

struct CloseArgs {
  const wchar_t* caption = nullptr;
  DWORD delay_ms = 0;
  std::wstring capture_path;  // 非空则先截图再关窗
  bool keep_open = false;
};

DWORD WINAPI AutoCloseProc(LPVOID param) {
  auto* args = static_cast<CloseArgs*>(param);
  ::Sleep(args->delay_ms);
  // 属性表是 #32770 对话框，标题就是 pszCaption
  HWND sheet = nullptr;
  for (int attempt = 0; attempt < 120; ++attempt) {
    sheet = ::FindWindowW(L"#32770", args->caption);
    if (sheet != nullptr) break;
    ::Sleep(50);
  }
  if (sheet == nullptr) {
    std::fwprintf(stderr, L"sheet window not found\n");
    return 1;
  }
  if (!args->capture_path.empty()) {
    HBITMAP bitmap = nullptr;
    if (CaptureWindow(sheet, &bitmap)) {
      if (SavePng(bitmap, args->capture_path)) {
        std::wprintf(L"captured %ls\n", args->capture_path.c_str());
      } else {
        std::fwprintf(stderr, L"SavePng failed\n");
      }
      ::DeleteObject(bitmap);
    } else {
      std::fwprintf(stderr, L"PrintWindow failed\n");
    }
  }
  if (!args->keep_open) {
    ::PostMessageW(sheet, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
  }
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::wstring file;
  std::wstring dll_path;
  int auto_close_ms = 0;
  bool verbose = false;
  std::wstring capture_path;
  bool keep_open = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--file" && i + 1 < argc) {
      file = argv[++i];
    } else if (arg == L"--dll" && i + 1 < argc) {
      dll_path = argv[++i];
    } else if (arg == L"--auto-close" && i + 1 < argc) {
      auto_close_ms = _wtoi(argv[++i]);
    } else if (arg == L"--capture" && i + 1 < argc) {
      capture_path = argv[++i];
    } else if (arg == L"--keep-open") {
      keep_open = true;
    } else if (arg == L"--verbose") {
      verbose = true;
    }
  }
  if (file.empty()) {
    std::fwprintf(stderr,
                  L"usage: proptest --file <apk> [--dll <apkprop.dll>] [--auto-close <ms>] "
                  L"[--verbose]\n");
    return 2;
  }
  if (dll_path.empty()) {
    const std::wstring dir = ExeDir();
    dll_path = dir + L"\\apkprop.dll";
  }

  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  HMODULE module = ::LoadLibraryW(dll_path.c_str());
  if (module == nullptr) {
    std::fwprintf(stderr, L"LoadLibrary failed for %ls (err=%lu)\n", dll_path.c_str(),
                  static_cast<unsigned long>(::GetLastError()));
    if (SUCCEEDED(com)) ::CoUninitialize();
    return 3;
  }
  using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
  auto get_class_object =
      reinterpret_cast<GetClassObjectFn>(::GetProcAddress(module, "DllGetClassObject"));
  if (get_class_object == nullptr) {
    std::fwprintf(stderr, L"DllGetClassObject not exported\n");
    return 3;
  }

  HRESULT hr = E_FAIL;
  apksx::ComPtr<IClassFactory> factory;
  hr = get_class_object(kPropClsid, IID_IClassFactory, reinterpret_cast<void**>(factory.Put()));
  if (verbose) std::wprintf(L"DllGetClassObject -> 0x%08lX\n", static_cast<unsigned long>(hr));
  if (FAILED(hr) || factory.Get() == nullptr) return 3;

  apksx::ComPtr<IShellPropSheetExt> page;
  hr = factory->CreateInstance(nullptr, IID_IShellPropSheetExt,
                               reinterpret_cast<void**>(page.Put()));
  if (verbose) std::wprintf(L"CreateInstance -> 0x%08lX\n", static_cast<unsigned long>(hr));
  if (FAILED(hr) || page.Get() == nullptr) return 3;

  apksx::ComPtr<IPersistFile> persist;
  hr = page->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(persist.Put()));
  if (FAILED(hr) || persist.Get() == nullptr) {
    std::fwprintf(stderr, L"IPersistFile not available (0x%08lX)\n",
                  static_cast<unsigned long>(hr));
    return 3;
  }
  hr = persist->Load(file.c_str(), STGM_READ);
  if (verbose) std::wprintf(L"IPersistFile::Load -> 0x%08lX\n", static_cast<unsigned long>(hr));

  PageCollector collector;
  hr = page->AddPages(AddPageProc, reinterpret_cast<LPARAM>(&collector));
  if (verbose) std::wprintf(L"AddPages -> 0x%08lX (%zu page)\n", static_cast<unsigned long>(hr),
                            collector.pages.size());
  if (FAILED(hr) || collector.pages.empty()) {
    std::fwprintf(stderr, L"AddPages produced no page (0x%08lX)\n",
                  static_cast<unsigned long>(hr));
    return 3;
  }

  std::wstring caption = file;
  const size_t slash = caption.find_last_of(L"\\/");
  if (slash != std::wstring::npos) caption = caption.substr(slash + 1);
  caption += L" 属性";

  PROPSHEETHEADERW header{};
  header.dwSize = sizeof(header);
  header.dwFlags = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW;
  header.hwndParent = nullptr;
  header.hInstance = module;
  header.pszCaption = caption.c_str();
  header.nPages = static_cast<UINT>(collector.pages.size());
  header.phpage = collector.pages.data();

  CloseArgs close_args;
  close_args.caption = caption.c_str();
  close_args.delay_ms = static_cast<DWORD>(auto_close_ms > 0 ? auto_close_ms : 0);
  close_args.capture_path = capture_path;
  close_args.keep_open = keep_open;
  HANDLE closer = nullptr;
  if (auto_close_ms > 0 || !capture_path.empty()) {
    closer = ::CreateThread(nullptr, 0, AutoCloseProc, &close_args, 0, nullptr);
  }

  ::SetLastError(0);
  const INT_PTR result = ::PropertySheetW(&header);
  const DWORD last_error = ::GetLastError();

  if (closer != nullptr) {
    ::TerminateThread(closer, 0);
    ::CloseHandle(closer);
  }
  if (verbose) {
    std::wprintf(L"PropertySheet -> %lld (last error %lu)\n", static_cast<long long>(result),
                 static_cast<unsigned long>(last_error));
  }
  if (SUCCEEDED(com)) ::CoUninitialize();
  return 0;
}

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
#include <cwchar>
#include <string>
#include <vector>

#include "core/com_ptr.h"
#include "core/json_lite.h"
#include "propsheet/page_list.h"
#include "propsheet/page_model.h"

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
  std::wstring capture_path;
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

// ---------------------------------------------------------------------------
// --list-json <json> --capture <png>
// 不经过属性表，直接用同一套 page_list 代码画一个 ListView 并截图。
// 用途：清单行/分组/主题的真实呈现（本 exe 带 comctl32 v6 清单，PowerShell 宿主没有）。
// ---------------------------------------------------------------------------
std::wstring g_capture_path;

std::string ReadAllBytes(const std::wstring& path) {
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) return std::string();
  std::string data;
  char buffer[65536];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) data.append(buffer, got);
  std::fclose(file);
  return data;
}

HBITMAP CaptureWindowRect(HWND hwnd) {
  RECT rect{};
  ::GetWindowRect(hwnd, &rect);
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) return nullptr;

  HDC screen = ::GetDC(nullptr);
  if (screen == nullptr) return nullptr;
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bitmap = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  HDC memory = ::CreateCompatibleDC(screen);
  if (bitmap == nullptr || memory == nullptr) {
    if (bitmap != nullptr) ::DeleteObject(bitmap);
    if (memory != nullptr) ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
    return nullptr;
  }
  HGDIOBJ old = ::SelectObject(memory, bitmap);
  // PW_RENDERFULLCONTENT(2) 走 DWM 取图，不依赖窗口是否在最前；失败再退回屏幕像素
  BOOL printed = ::PrintWindow(hwnd, memory, 2);
  std::wprintf(L"capture rect=%ld,%ld %ldx%ld printwindow=%d\n",\
               static_cast<long>(rect.left), static_cast<long>(rect.top),\
               static_cast<long>(width), static_cast<long>(height), static_cast<int>(printed));
  if (!printed) {
    ::BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY);
  }
  ::SelectObject(memory, old);
  ::DeleteDC(memory);
  ::ReleaseDC(nullptr, screen);
  return bitmap;
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* list = reinterpret_cast<HWND>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (message) {
    case WM_SIZE: {
      if (list != nullptr) {
        RECT client{};
        ::GetClientRect(hwnd, &client);
        const int width = client.right - 16;
        const int height = client.bottom - 16;
        ::MoveWindow(list, 8, 8, width, height, TRUE);
        apksx::ApplyListColumns(list, width, 96);
      }
      return 0;
    }
    case WM_TIMER: {
      ::KillTimer(hwnd, static_cast<UINT_PTR>(wparam));
      // 抓屏幕像素前必须确保不被其它窗口遮住：临时置顶（SetForegroundWindow 在无焦点权时无效）
      ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
      ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
      ::Sleep(250);
      if (!g_capture_path.empty()) {
        HBITMAP bitmap = CaptureWindowRect(hwnd);
        if (bitmap != nullptr) {
          SavePng(bitmap, g_capture_path);
          ::DeleteObject(bitmap);
        }
      }
      ::SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      ::PostQuitMessage(0);
      return 0;
    }
    case WM_DESTROY:
      ::PostQuitMessage(0);
      return 0;
    default: break;
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

int RunListPreview(const std::wstring& json_path, const std::wstring& capture_path) {
  const std::string text = ReadAllBytes(json_path);
  if (text.empty()) {
    std::fwprintf(stderr, L"cannot read %s\n", json_path.c_str());
    return 3;
  }
  apksx::json::ValuePtr doc;
  apksx::Error err;
  if (!apksx::json::Parse(text, &doc, &err) || doc == nullptr) {
    std::fwprintf(stderr, L"bad json: %s\n", err.message.c_str());
    return 3;
  }
  apksx::PageModel model;
  if (!apksx::BuildPageModel(*doc, L"preview.apk", &model, &err)) {
    std::fwprintf(stderr, L"model failed: %s\n", err.message.c_str());
    return 3;
  }

  const HINSTANCE instance = ::GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = PreviewProc;
  wc.hInstance = instance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = L"apksx.listpreview";
  if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 3;

  HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"list preview",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN, 120, 120, 560,
                                720, nullptr, nullptr, instance, nullptr);
  if (hwnd == nullptr) return 3;

  HWND list = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
                                    LVS_SHOWSELALWAYS,
                                8, 8, 540, 700, hwnd, nullptr, instance, nullptr);
  if (list == nullptr) return 3;
  ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(list));
  ::SendMessageW(list, WM_SETFONT,
                 reinterpret_cast<WPARAM>(::GetStockObject(DEFAULT_GUI_FONT)), FALSE);

  apksx::ConfigureListView(list);
  LVCOLUMNW column{};
  column.mask = LVCF_TEXT | LVCF_SUBITEM | LVCF_FMT;
  column.fmt = LVCFMT_LEFT;
  column.pszText = const_cast<wchar_t*>(L"属性");
  column.iSubItem = 0;
  ListView_InsertColumn(list, 0, &column);
  column.pszText = const_cast<wchar_t*>(L"值");
  column.iSubItem = 1;
  ListView_InsertColumn(list, 1, &column);
  apksx::PopulateListView(list, model);

  ::SendMessageW(hwnd, WM_SIZE, 0, 0);
  ::ShowWindow(hwnd, SW_SHOW);
  ::UpdateWindow(hwnd);

  g_capture_path = capture_path;
  ::SetTimer(hwnd, 1, 700, nullptr);
  MSG message{};
  while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&message);
    ::DispatchMessageW(&message);
  }
  return 0;
}

int wmain(int argc, wchar_t** argv) {
  std::wstring file;
  std::wstring dll_path;
  int auto_close_ms = 0;
  bool verbose = false;
  std::wstring capture_path;
  std::wstring list_json;
  bool keep_open = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--file" && i + 1 < argc) {
      file = argv[++i];
    } else if (arg == L"--dll" && i + 1 < argc) {
      dll_path = argv[++i];
    } else if (arg == L"--auto-close" && i + 1 < argc) {
      auto_close_ms = _wtoi(argv[++i]);
    } else if (arg == L"--list-json" && i + 1 < argc) {
      list_json = argv[++i];
    } else if (arg == L"--capture" && i + 1 < argc) {
      capture_path = argv[++i];
    } else if (arg == L"--keep-open") {
      keep_open = true;
    } else if (arg == L"--verbose") {
      verbose = true;
    }
  }
  if (!list_json.empty()) return RunListPreview(list_json, capture_path);
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

  // 属性表要 comctl32；没有 InitCommonControlsEx 时 PropertySheetW 会直接返回 -1
  INITCOMMONCONTROLSEX controls{};
  controls.dwSize = sizeof(controls);
  controls.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
  ::InitCommonControlsEx(&controls);

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

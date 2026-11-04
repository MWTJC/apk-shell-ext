// `apkprop.dll` 的窗口逻辑：IShellPropSheetExt + IPersistFile + IShellExtInit。
//
// 2026-11-04 重构（用户：嵌入现有面板时自绘不是首选）：
//   页面全部用**原生控件**——STATIC（图标/名称/包名/版本/状态）+ SysListView32
//   （两列「属性/值」+ 系统分组，等同资源管理器"详细信息"页的形态）+ BUTTON。
//   因此这里没有任何自绘代码：配色、选中、滚动、截断 tooltip、键盘、无障碍都交给系统；
//   本文件只做「建控件 + 排版 + 起 worker + 塞数据 + 复制」。
//
// 生命周期：AddPages 只登记页面；WM_INITDIALOG 建控件并起线程；线程跑 apkinfo.exe 后
// PostMessage 回填；WM_DESTROY 置取消并放掉 Job（worker 随之被杀）。
#include "propsheet/apkprop.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/apk_icon.h"
#include "core/common.h"
#include "core/json_lite.h"
#include "core/log.h"
#include "core/thumb_cache.h"
#include "core/wic_image.h"
#include "core/zip_reader.h"
#include "propsheet/page_list.h"
#include "propsheet/page_layout.h"
#include "propsheet/page_model.h"
#include "propsheet/theme.h"
#include "propsheet/worker_client.h"

namespace {

constexpr UINT kMsgInfoDone = WM_APP + 1;
constexpr UINT kMsgHashDone = WM_APP + 2;

// 控件 ID（全部代码创建，不需要改 .rc 模板）
constexpr int kIdList = 1001;
constexpr int kIdHash = 1002;
constexpr int kIdCopyJson = 1003;
constexpr int kIdRetry = 1004;
constexpr int kIdStatus = 1005;
constexpr int kIdIcon = 1006;
constexpr int kIdLabel = 1007;
constexpr int kIdPackage = 1008;
constexpr int kIdVersion = 1009;
constexpr int kIdSeparator = 1010;

constexpr int kMenuCopyValue = 2001;
constexpr int kMenuCopyPair = 2002;
constexpr int kMenuCopyJson = 2003;

std::wstring FileNameOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool CopyTextToClipboard(HWND hwnd, const std::wstring& text) {
  if (text.empty()) return false;
  if (!::OpenClipboard(hwnd)) return false;
  ::EmptyClipboard();
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
  bool ok = false;
  if (memory != nullptr) {
    void* data = ::GlobalLock(memory);
    if (data != nullptr) {
      std::memcpy(data, text.c_str(), bytes);
      ::GlobalUnlock(memory);
      if (::SetClipboardData(CF_UNICODETEXT, memory) != nullptr) ok = true;
    }
    if (!ok) ::GlobalFree(memory);
  }
  ::CloseClipboard();
  return ok;
}

// 头部图标：缩略图缓存优先；否则在小工作线程里解一个图标条目（只读清单 + 一个条目）
HBITMAP DecodeIconFromZip(const std::wstring& path, int size) {
  apksx::ZipReader zip;
  apksx::Error err;
  if (!zip.Open(path, &err)) return nullptr;
  const apksx::Deadline deadline(1500);
  apksx::IconLocation icon;
  apksx::ManifestInfo manifest;
  if (!apksx::LocateApkIconInZip(zip, &icon, &manifest, &err, &deadline)) return nullptr;
  const apksx::ZipEntry* entry = zip.Find(icon.entry_path);
  if (entry == nullptr) return nullptr;
  std::vector<uint8_t> bytes;
  if (!zip.Read(*entry, apksx::kMaxIconRead, &bytes, &err)) return nullptr;
  apksx::DecodedImage image;
  if (!image.DecodeFromMemory(bytes, 512, &err)) return nullptr;
  HBITMAP bitmap = nullptr;
  uint32_t cx = 0;
  uint32_t cy = 0;
  if (!image.CreateHBitmap(static_cast<uint32_t>(size), &bitmap, &cx, &cy, &err)) return nullptr;
  return bitmap;
}

HBITMAP LoadHeaderIcon(const std::wstring& path, int size) {
  apksx::CacheLookup lookup;
  if (apksx::ThumbCache::Lookup(path, &lookup) && lookup.hit) {
    apksx::DecodedImage image;
    apksx::Error err;
    if (apksx::DecodePngFile(lookup.png_path, 256, &image, &err)) {
      HBITMAP bitmap = nullptr;
      uint32_t cx = 0;
      uint32_t cy = 0;
      if (image.CreateHBitmap(static_cast<uint32_t>(size), &bitmap, &cx, &cy, &err)) {
        return bitmap;
      }
    }
  }
  return DecodeIconFromZip(path, size);
}

class PropPage;

struct PendingResult {
  enum class Kind { Info, Hash };
  Kind kind = Kind::Info;
  apksx::PageModel model;
  HBITMAP icon = nullptr;
  std::wstring sha256;
  bool ok = false;
  std::wstring footer;
  std::string raw_json;
};

struct PageState : std::enable_shared_from_this<PageState> {
  HWND hwnd = nullptr;
  HWND list = nullptr;
  HWND icon_view = nullptr;
  HWND label_view = nullptr;
  HWND package_view = nullptr;
  HWND version_view = nullptr;
  HWND separator = nullptr;
  HWND status_view = nullptr;
  HWND hash_button = nullptr;
  HWND copy_button = nullptr;
  HWND retry_button = nullptr;

  std::wstring path;
  std::wstring file_name;

  std::mutex mutex;
  std::unique_ptr<PendingResult> pending;
  std::atomic<bool> cancel{false};

  bool worker_running = false;
  bool hash_running = false;
  bool info_ok = false;

  PropPage* owner = nullptr;  // 组件对象；WM_DESTROY 时用它放掉 AddPages 里的 AddRef

  apksx::PageModel model;
  apksx::PageTheme theme;
  HBITMAP icon = nullptr;
  std::wstring footer = L"正在解析…";
  std::string raw_json;

  HWND Window() {
    std::lock_guard<std::mutex> guard(mutex);
    return hwnd;
  }
};

void Relayout(HWND hwnd);
void RefreshView(HWND hwnd);

// ------------------------------------------------------------------ 线程侧

void WorkerThread(std::shared_ptr<PageState> state, bool want_hash, std::wstring path) {
  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  const ULONGLONG started = ::GetTickCount64();
  apksx::WorkerOutput output;
  std::wstring exe;
  if (!apksx::FindWorkerExe(&exe)) {
    output.status = apksx::WorkerStatus::Missing;
    output.message = L"解析器缺失：apkinfo.exe 不在扩展目录里";
  } else {
    output = apksx::RunWorker(exe, path, want_hash, want_hash ? 30 : 8, &state->cancel);
  }

  if (state->cancel.load() || output.status == apksx::WorkerStatus::Cancelled) {
    if (SUCCEEDED(com)) ::CoUninitialize();
    return;
  }

  auto result = std::make_unique<PendingResult>();
  result->kind = want_hash ? PendingResult::Kind::Hash : PendingResult::Kind::Info;
  const ULONGLONG elapsed = ::GetTickCount64() - started;

  if (output.status == apksx::WorkerStatus::Ok) {
    apksx::json::ValuePtr doc;
    apksx::Error err;
    if (!apksx::json::Parse(output.stdout_text, &doc, &err) || doc == nullptr) {
      output.status = apksx::WorkerStatus::BadOutput;
      output.message = L"解析输出不是合法 JSON";
    } else if (want_hash) {
      const apksx::json::Value* file = doc->Find("file");
      const apksx::json::Value* sha = file == nullptr ? nullptr : file->Find("sha256");
      std::wstring wide;
      if (sha != nullptr && sha->is_string() && apksx::Utf8ToWide(sha->as_string(), &wide)) {
        result->sha256 = wide;
        result->ok = true;
      } else {
        output.status = apksx::WorkerStatus::BadOutput;
        output.message = L"哈希计算没有返回 sha256";
      }
    } else if (apksx::BuildPageModel(*doc, state->file_name, &result->model, &err)) {
      result->ok = true;
    } else {
      output.status = apksx::WorkerStatus::BadOutput;
      output.message = L"解析输出结构不认识";
    }
    if (result->ok) result->raw_json = output.stdout_text;
  }

  if (result->ok) {
    wchar_t buf[96];
    if (want_hash) {
      _snwprintf_s(buf, _TRUNCATE, L"哈希已计算（%llu ms）", elapsed);
    } else {
      _snwprintf_s(buf, _TRUNCATE, L"解析完成 · %llu ms", elapsed);
    }
    result->footer = buf;
    if (!want_hash) {
      int icon_px = 32;
      if (const HWND window = state->Window(); window != nullptr) {
        const UINT dpi = ::GetDpiForWindow(window);
        if (dpi != 0) icon_px = ::MulDiv(32, static_cast<int>(dpi), 96);
      }
      result->icon = LoadHeaderIcon(path, icon_px);
    }
  } else {
    result->footer = output.message.empty() ? L"解析失败" : output.message;
  }

  if (SUCCEEDED(com)) ::CoUninitialize();

  {
    std::lock_guard<std::mutex> guard(state->mutex);
    if (state->hwnd == nullptr) {
      if (result->icon != nullptr) ::DeleteObject(result->icon);
      return;
    }
    state->pending = std::move(result);
    ::PostMessageW(state->hwnd, want_hash ? kMsgHashDone : kMsgInfoDone, 0, 0);
  }
}

void StartWorker(HWND hwnd, const std::shared_ptr<PageState>& state, bool want_hash) {
  (void)hwnd;
  state->cancel.store(false);
  if (want_hash) {
    state->hash_running = true;
    state->footer = L"正在计算 SHA-256…";
  } else {
    state->worker_running = true;
    state->info_ok = false;
    state->footer = L"正在解析…";
  }
  RefreshView(state->hwnd);
  std::thread worker(WorkerThread, state, want_hash, state->path);
  worker.detach();
}

// ------------------------------------------------------------------ 视图

void ApplyFonts(PageState* state) {
  HWND controls[] = {state->list,       state->icon_view,  state->package_view,
                     state->version_view, state->status_view, state->hash_button,
                     state->copy_button,  state->retry_button};
  for (HWND control : controls) {
    if (control != nullptr) {
      ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->theme.font), TRUE);
    }
  }
  if (state->label_view != nullptr) {
    ::SendMessageW(state->label_view, WM_SETFONT,
                   reinterpret_cast<WPARAM>(state->theme.font_bold), TRUE);
  }
}

void SetWindowTextSafe(HWND control, const std::wstring& text) {
  if (control != nullptr) ::SetWindowTextW(control, text.c_str());
}

void Relayout(HWND hwnd) {
  auto* state = reinterpret_cast<PageState*>(::GetWindowLongPtrW(hwnd, DWLP_USER));
  if (state == nullptr) return;

  RECT client{};
  ::GetClientRect(hwnd, &client);
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;

  // 布局数学在 page_layout.cpp 里（纯函数，可按 DPI 单测），这里只搬矩形
  const apksx::PageLayout layout = apksx::ComputePageLayout(
      state->theme.dpi, width, height, ::IsWindowVisible(state->hash_button) != FALSE,
      ::IsWindowVisible(state->retry_button) != FALSE);
  const auto width_of = [](const RECT& rect) { return rect.right - rect.left; };
  const auto place = [](HWND control, const RECT& rect) {
    if (control == nullptr) return;
    ::MoveWindow(control, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, TRUE);
  };

  place(state->icon_view, layout.icon);
  place(state->label_view, layout.label);
  place(state->package_view, layout.package);
  place(state->version_view, layout.version);
  place(state->separator, layout.separator);
  place(state->copy_button, layout.copy_button);
  place(state->hash_button, layout.hash_button);
  place(state->retry_button, layout.retry_button);
  place(state->status_view, layout.status);
  place(state->list, layout.list);

  apksx::ApplyListColumns(state->list, width_of(layout.list), state->theme.dpi);
}

void RefreshView(HWND hwnd) {
  auto* state = reinterpret_cast<PageState*>(::GetWindowLongPtrW(hwnd, DWLP_USER));
  if (state == nullptr) return;

  SetWindowTextSafe(state->label_view,
                    state->model.label.empty() ? state->file_name : state->model.label);
  SetWindowTextSafe(state->package_view, state->model.package);
  SetWindowTextSafe(state->version_view, state->model.version_line);
  SetWindowTextSafe(state->status_view, state->footer);

  apksx::PopulateListView(state->list, state->model);

  const bool show_hash = state->info_ok && !state->hash_running;
  const bool show_retry = !state->worker_running && !state->info_ok;
  ::ShowWindow(state->icon_view, state->icon == nullptr ? SW_HIDE : SW_SHOW);
  ::ShowWindow(state->hash_button, show_hash ? SW_SHOW : SW_HIDE);
  ::ShowWindow(state->retry_button, show_retry ? SW_SHOW : SW_HIDE);
  ::EnableWindow(state->copy_button, state->raw_json.empty() ? FALSE : TRUE);

  Relayout(hwnd);
}

// ------------------------------------------------------------------ 交互

void CopySelected(HWND hwnd, bool with_key) {
  auto* state = reinterpret_cast<PageState*>(::GetWindowLongPtrW(hwnd, DWLP_USER));
  if (state == nullptr) return;
  const int index = apksx::SelectedListIndex(state->list);
  if (index < 0) return;
  const std::wstring key = apksx::ListItemText(state->list, index, 0);
  const std::wstring value = apksx::ListItemText(state->list, index, 1);
  std::wstring text = value.empty() ? key : value;
  if (with_key && !key.empty() && !value.empty()) text = key + L": " + value;
  if (CopyTextToClipboard(hwnd, text)) {
    state->footer = L"已复制到剪贴板";
    SetWindowTextSafe(state->status_view, state->footer);
  }
}

void CopyAllJson(HWND hwnd) {
  auto* state = reinterpret_cast<PageState*>(::GetWindowLongPtrW(hwnd, DWLP_USER));
  if (state == nullptr) return;
  std::wstring wide;
  if (!state->raw_json.empty() && apksx::Utf8ToWide(state->raw_json, &wide)) {
    if (CopyTextToClipboard(hwnd, wide)) {
      state->footer = L"已复制 JSON 到剪贴板";
      SetWindowTextSafe(state->status_view, state->footer);
    }
  }
}

void StartHash(HWND hwnd) {
  auto* state = reinterpret_cast<PageState*>(::GetWindowLongPtrW(hwnd, DWLP_USER));
  if (state == nullptr) return;
  if (!state->info_ok || state->hash_running || state->worker_running) return;
  StartWorker(hwnd, state->shared_from_this(), true);
}

void ShowListMenu(HWND hwnd, int screen_x, int screen_y) {
  auto* state = reinterpret_cast<PageState*>(::GetWindowLongPtrW(hwnd, DWLP_USER));
  if (state == nullptr) return;

  POINT client{screen_x, screen_y};
  ::ScreenToClient(state->list, &client);
  LVHITTESTINFO hit{};
  hit.pt = client;
  const int index = ListView_HitTest(state->list, &hit);
  if (index >= 0) {
    ListView_SetItemState(state->list, index, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
  }

  HMENU menu = ::CreatePopupMenu();
  if (menu == nullptr) return;
  ::AppendMenuW(menu, MF_STRING, kMenuCopyValue, L"复制值");
  ::AppendMenuW(menu, MF_STRING, kMenuCopyPair, L"复制 \u201C属性: 值\u201D");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING | (state->raw_json.empty() ? MF_GRAYED : 0U), kMenuCopyJson,
                L"复制全部为 JSON");

  const int command = static_cast<int>(
      ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_x, screen_y, 0, hwnd, nullptr));
  ::DestroyMenu(menu);

  switch (command) {
    case kMenuCopyValue: CopySelected(hwnd, false); break;
    case kMenuCopyPair: CopySelected(hwnd, true); break;
    case kMenuCopyJson: CopyAllJson(hwnd); break;
    default: break;
  }
}

// ------------------------------------------------------------------ 控件创建

HWND CreateStatic(HWND parent, int id, DWORD style) {
  return ::CreateWindowExW(0, L"Static", L"", WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           ::GetModuleHandleW(nullptr), nullptr);
}

void CreateControls(HWND hwnd, PageState* state) {
  state->icon_view = CreateStatic(hwnd, kIdIcon, SS_BITMAP | SS_CENTERIMAGE);
  state->label_view = CreateStatic(hwnd, kIdLabel, SS_LEFT | SS_ENDELLIPSIS);
  state->package_view = CreateStatic(hwnd, kIdPackage, SS_LEFT | SS_ENDELLIPSIS);
  state->version_view = CreateStatic(hwnd, kIdVersion, SS_LEFT | SS_ENDELLIPSIS);
  state->separator = CreateStatic(hwnd, kIdSeparator, SS_ETCHEDHORZ);

  state->list = ::CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
          LVS_NOSORTHEADER,
      0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdList)),
      ::GetModuleHandleW(nullptr), nullptr);
  apksx::ConfigureListView(state->list);
  LVCOLUMNW column{};
  column.mask = LVCF_TEXT | LVCF_SUBITEM | LVCF_FMT;
  column.fmt = LVCFMT_LEFT;
  column.pszText = const_cast<wchar_t*>(L"属性");
  column.iSubItem = 0;
  ListView_InsertColumn(state->list, 0, &column);
  column.pszText = const_cast<wchar_t*>(L"值");
  column.iSubItem = 1;
  ListView_InsertColumn(state->list, 1, &column);

  state->status_view = CreateStatic(hwnd, kIdStatus, SS_LEFT | SS_ENDELLIPSIS);

  const DWORD button_style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
  state->hash_button = ::CreateWindowExW(
      0, L"Button", L"计算 SHA-256", button_style, 0, 0, 0, 0, hwnd,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdHash)), ::GetModuleHandleW(nullptr), nullptr);
  state->copy_button = ::CreateWindowExW(
      0, L"Button", L"复制全部为 JSON", button_style, 0, 0, 0, 0, hwnd,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCopyJson)), ::GetModuleHandleW(nullptr),
      nullptr);
  state->retry_button = ::CreateWindowExW(
      0, L"Button", L"重试", button_style, 0, 0, 0, 0, hwnd,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRetry)), ::GetModuleHandleW(nullptr), nullptr);
  ::ShowWindow(state->retry_button, SW_HIDE);
  ::EnableWindow(state->copy_button, FALSE);
}

void ApplyPending(HWND hwnd);

INT_PTR CALLBACK DlgProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

class PropPage final : public IShellPropSheetExt, public IPersistFile, public IShellExtInit {
 public:
  PropPage() { IncrementServerLock(); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IShellPropSheetExt) {
      *ppv = static_cast<IShellPropSheetExt*>(this);
    } else if (riid == IID_IPersistFile || riid == IID_IPersist) {
      *ppv = static_cast<IPersistFile*>(this);
    } else if (riid == IID_IShellExtInit) {
      // 实测：这个 Windows build 在建页之前先 QI 它，拿不到就整个跳过 handler
      *ppv = static_cast<IShellExtInit*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(::InterlockedIncrement(&refs_));
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const long remaining = ::InterlockedDecrement(&refs_);
    if (remaining == 0) {
      DecrementServerLock();
      delete this;
    }
    return static_cast<ULONG>(remaining);
  }

  HRESULT STDMETHODCALLTYPE AddPages(LPFNADDPROPSHEETPAGE add_page, LPARAM lparam) override {
    if (add_page == nullptr) return E_POINTER;
    try {
      if (state_ == nullptr) {
        state_ = std::make_shared<PageState>();
        state_->path = path_;
        state_->file_name = FileNameOf(path_);
      }

      PROPSHEETPAGEW page{};
      page.dwSize = sizeof(page);
      page.dwFlags = PSP_DEFAULT | PSP_USETITLE;
      page.hInstance = g_module;
      page.pszTemplate = MAKEINTRESOURCEW(IDD_APKPROP);
      page.pszTitle = MAKEINTRESOURCEW(IDS_PAGE_TITLE);
      page.pfnDlgProc = DlgProc;
      page.lParam = reinterpret_cast<LPARAM>(this);

      HPROPSHEETPAGE handle = ::CreatePropertySheetPageW(&page);
      if (handle == nullptr) return E_FAIL;
      AddRef();
      if (!add_page(handle, lparam)) {
        ::DestroyPropertySheetPage(handle);
        Release();
        return E_FAIL;
      }
      return S_OK;
    } catch (...) {
      return E_UNEXPECTED;
    }
  }

  HRESULT STDMETHODCALLTYPE ReplacePage(UINT page_id, LPFNADDPROPSHEETPAGE add_page,
                                        LPARAM lparam) override {
    (void)page_id;
    (void)add_page;
    (void)lparam;
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE GetClassID(CLSID* clsid) override {
    if (clsid == nullptr) return E_POINTER;
    *clsid = CLSID_ApkPropertyPage;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE IsDirty() override { return S_FALSE; }

  HRESULT STDMETHODCALLTYPE Load(LPCOLESTR filename, DWORD mode) override {
    (void)mode;
    if (filename == nullptr) return E_INVALIDARG;
    path_ = filename;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Save(LPCOLESTR filename, BOOL remember) override {
    (void)filename;
    (void)remember;
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE SaveCompleted(LPCOLESTR filename) override {
    (void)filename;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetCurFile(LPOLESTR* filename) override {
    if (filename == nullptr) return E_POINTER;
    *filename = nullptr;
    return S_FALSE;
  }

  HRESULT STDMETHODCALLTYPE Initialize(PCIDLIST_ABSOLUTE folder, IDataObject* data,
                                       HKEY key) override {
    (void)folder;
    (void)key;
    if (data == nullptr) return E_INVALIDARG;
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&format, &medium))) return E_INVALIDARG;
    HDROP drop = static_cast<HDROP>(::GlobalLock(medium.hGlobal));
    std::wstring path;
    if (drop != nullptr) {
      const UINT length = ::DragQueryFileW(drop, 0, nullptr, 0);
      if (length > 0) {
        std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
        if (::DragQueryFileW(drop, 0, buffer.data(), static_cast<UINT>(buffer.size())) > 0) {
          path.assign(buffer.data());
        }
      }
      ::GlobalUnlock(medium.hGlobal);
    }
    ::ReleaseStgMedium(&medium);
    if (path.empty()) return E_INVALIDARG;
    path_ = path;
    return S_OK;
  }

  const std::shared_ptr<PageState>& state() const { return state_; }
  const std::wstring& path() const { return path_; }

 private:
  ~PropPage() = default;
  volatile long refs_ = 1;
  std::wstring path_;
  std::shared_ptr<PageState> state_;
};

INT_PTR CALLBACK DlgProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<PageState*>(::GetWindowLongPtrW(hwnd, DWLP_USER));

  switch (message) {
    case WM_INITDIALOG: {
      auto* psp = reinterpret_cast<PROPSHEETPAGEW*>(lparam);
      auto* page = psp == nullptr ? nullptr : reinterpret_cast<PropPage*>(psp->lParam);
      if (page == nullptr) return FALSE;
      state = page->state().get();
      if (state == nullptr) return FALSE;
      state->owner = page;
      ::SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(state));

      {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->hwnd = hwnd;
        state->cancel.store(false);
      }
      (void)apksx::LoadTheme(&state->theme, hwnd);
      CreateControls(hwnd, state);
      ApplyFonts(state);
      state->model = apksx::PageModel();
      state->model.label = state->file_name;
      RefreshView(hwnd);
      StartWorker(hwnd, page->state(), false);
      return TRUE;
    }

    case WM_SIZE:
      Relayout(hwnd);
      break;

    case WM_COMMAND: {
      if (state == nullptr) break;
      switch (LOWORD(wparam)) {
        case kIdHash: StartHash(hwnd); break;
        case kIdCopyJson: CopyAllJson(hwnd); break;
        case kIdRetry: StartWorker(hwnd, state->shared_from_this(), false); break;
        default: break;
      }
      break;
    }

    case WM_NOTIFY: {
      if (state == nullptr) break;
      auto* header = reinterpret_cast<NMHDR*>(lparam);
      if (header == nullptr || header->hwndFrom != state->list) break;
      if (header->code == NM_RCLICK) {
        POINT pt{};
        ::GetCursorPos(&pt);
        ShowListMenu(hwnd, pt.x, pt.y);
      } else if (header->code == NM_DBLCLK) {
        CopySelected(hwnd, false);
      } else if (header->code == LVN_KEYDOWN) {
        auto* key = reinterpret_cast<NMLVKEYDOWN*>(lparam);
        if (key != nullptr && key->wVKey == 'C' && (::GetKeyState(VK_CONTROL) & 0x8000) != 0) {
          CopySelected(hwnd, false);
        }
      }
      break;
    }

    case kMsgInfoDone:
    case kMsgHashDone:
      ApplyPending(hwnd);
      break;

    // 系统外观 / DPI 变化：字体重新问宿主；控件由系统按主题重画，我们只做重排
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
    case WM_DPICHANGED: {
      if (state == nullptr) break;
      apksx::DestroyTheme(&state->theme);
      (void)apksx::LoadTheme(&state->theme, hwnd);
      ApplyFonts(state);
      Relayout(hwnd);
      ::InvalidateRect(hwnd, nullptr, TRUE);
      break;
    }

    case WM_DESTROY: {
      if (state == nullptr) break;
      // 先拿住一份 shared_ptr：放掉 PropPage 的引用后，state 由这里保活到函数结束
      const std::shared_ptr<PageState> keep = state->shared_from_this();
      PageState* shared_state = state;
      if (shared_state != nullptr) {
        {
          std::lock_guard<std::mutex> guard(shared_state->mutex);
          shared_state->hwnd = nullptr;
        }
        shared_state->cancel.store(true);
        if (shared_state->icon != nullptr) {
          ::DeleteObject(shared_state->icon);
          shared_state->icon = nullptr;
        }
        apksx::DestroyTheme(&shared_state->theme);
      }
      ::SetWindowLongPtrW(hwnd, DWLP_USER, 0);
      if (state->owner != nullptr) {
        PropPage* owner = state->owner;
        state->owner = nullptr;
        owner->Release();  // 可能在这里销毁 PropPage（它持有的 shared_ptr 会在 keep 之后才释放）
      }
      break;
    }

    default: break;
  }
  return FALSE;
}

void ApplyPending(HWND hwnd) {
  auto* state = reinterpret_cast<PageState*>(::GetWindowLongPtrW(hwnd, DWLP_USER));
  if (state == nullptr) return;

  std::unique_ptr<PendingResult> result;
  {
    std::lock_guard<std::mutex> guard(state->mutex);
    result = std::move(state->pending);
  }
  if (result == nullptr) return;

  if (result->kind == PendingResult::Kind::Info) {
    state->worker_running = false;
    state->info_ok = result->ok;
    if (result->ok) {
      state->raw_json = result->raw_json;
      state->model = std::move(result->model);
      if (result->icon != nullptr) {
        if (state->icon != nullptr) ::DeleteObject(state->icon);
        state->icon = result->icon;
        result->icon = nullptr;
        ::SendMessageW(state->icon_view, STM_SETIMAGE, IMAGE_BITMAP,
                       reinterpret_cast<LPARAM>(state->icon));
      }
    } else {
      std::vector<std::wstring> lines;
      lines.push_back(result->footer);
      lines.push_back(L"可以点「重试」再解析一次。");
      apksx::PageModel model;
      apksx::BuildErrorModel(state->file_name, lines, &model);
      state->model = std::move(model);
      state->raw_json.clear();
    }
  } else {
    state->hash_running = false;
    if (result->ok) apksx::SetSha256(&state->model, result->sha256);
  }
  state->footer = result->footer;
  RefreshView(hwnd);
  // 算完哈希后滚到那一行：结果就在眼前，不用自己找
  if (result->kind == PendingResult::Kind::Hash && result->ok) {
    apksx::ScrollListRowIntoView(state->list, apksx::FindListRowByKey(state->list, L"SHA-256"));
  }
}

}  // namespace

HRESULT CreatePropertyPage(REFIID riid, void** ppv) {
  if (ppv == nullptr) return E_POINTER;
  *ppv = nullptr;
  auto* page = new (std::nothrow) PropPage();
  if (page == nullptr) return E_OUTOFMEMORY;
  const HRESULT hr = page->QueryInterface(riid, ppv);
  page->Release();
  return hr;
}

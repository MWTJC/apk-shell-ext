// `apkprop.dll` 的全部窗口逻辑：IShellPropSheetExt + IPersistFile + 自绘属性页。
//
// 生命周期（docs/m3-property-page.md §4）：
//   AddPages  → 只登记页面（零 IO），AddRef 自己等 WM_DESTROY 时 Release
//   WM_INITDIALOG → 建骨架 → 起线程 → SetWindowLongPtr(DWLP_USER, PageState*)
//   线程     → apkinfo.exe（Job 里）→ 解析 JSON 成 PageModel → PostMessage(WM_APP+n)
//   WM_APP+n → UI 线程收模型，重排 + 重画
//   WM_DESTROY → cancel=true + hwnd=nullptr（都在 state->mutex 里），线程自己收尾
#include "propsheet/apkprop.h"

#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>
#include <commctrl.h>

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
#include "propsheet/page_model.h"
#include "propsheet/page_view.h"
#include "propsheet/theme.h"
#include "propsheet/worker_client.h"

namespace {

constexpr UINT kMsgInfoDone = WM_APP + 1;
constexpr UINT kMsgHashDone = WM_APP + 2;

constexpr UINT_PTR kTooltipTimerId = 1;
constexpr UINT kTooltipDwellMs = 600;      // 悬停多久才出 tooltip（原生行为）

constexpr int kMenuCopyValue = 1001;
constexpr int kMenuCopyPair = 1002;
constexpr int kMenuCopyJson = 1003;

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

// 头部图标：
//   1) 先读自建缩略图缓存（命中就不碰原文件）；
//   2) 未命中时在小工作线程里解一个图标条目 —— 与 apkthumb 同一套 core 代码，
//      只读中央目录 + 一个条目，不做全量解析（ADR-0003 的边界仍然成立）。
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

struct PendingResult {
  enum class Kind { Info, Hash };
  Kind kind = Kind::Info;
  apksx::PageModel model;
  HBITMAP icon = nullptr;  // 所有权转交给 UI 线程
  std::wstring sha256;
  bool ok = false;
  std::wstring footer;
  std::string raw_json;
};

struct PageState : std::enable_shared_from_this<PageState> {
  HWND hwnd = nullptr;
  HWND tooltip = nullptr;
  std::wstring path;
  std::wstring file_name;

  std::mutex mutex;  // 保护 hwnd / pending（线程与 UI 线程共享）
  std::unique_ptr<PendingResult> pending;
  std::atomic<bool> cancel{false};

  bool worker_running = false;
  bool hash_running = false;
  bool info_ok = false;

  apksx::PageModel model;
  apksx::PageTheme theme;
  apksx::PageView view;
  HBITMAP icon = nullptr;

  int scroll_y = 0;
  int hot = -1;
  int selected = -1;
  bool tracking_mouse = false;
  bool hand_cursor = false;
  int last_mouse_x = 0;
  int last_mouse_y = 0;
  bool tooltip_visible = false;

  // 双缓冲：只在尺寸变化时重建（快速划过时每秒几十次重绘也不会闪）
  HDC buffer_dc = nullptr;
  HBITMAP buffer_bitmap = nullptr;
  int buffer_width = 0;
  int buffer_height = 0;
  std::wstring footer = L"正在解析…";
  std::string raw_json;
  ULONGLONG started_ms = 0;

  // UI 线程用：安全地读 hwnd（可能已被 WM_DESTROY 清掉）
  HWND Window() {
    std::lock_guard<std::mutex> guard(mutex);
    return hwnd;
  }
};

void Relayout(HWND hwnd, PageState* state);
void UpdateScrollbar(HWND hwnd, PageState* state);

// ------------------------------------------------------------------ 线程侧

void WorkerThread(std::shared_ptr<PageState> state, bool want_hash, std::wstring path) {
  // WIC（图标解码）需要 COM；线程自己初始化，不依赖 explorer 的套间
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
    return;  // 页面已关：模型、图标都不需要了
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
    } else {
      if (want_hash) {
        const apksx::json::Value* file = doc->Find("file");
        const apksx::json::Value* sha = file == nullptr ? nullptr : file->Find("sha256");
        if (sha != nullptr && sha->is_string()) {
          std::wstring wide;
          if (apksx::Utf8ToWide(sha->as_string(), &wide)) {
            result->sha256 = wide;
            result->ok = true;
          }
        }
        if (!result->ok) {
          output.status = apksx::WorkerStatus::BadOutput;
          output.message = L"哈希计算没有返回 sha256";
        }
      } else {
        if (apksx::BuildPageModel(*doc, state->file_name, &result->model, &err)) {
          result->ok = true;
        } else {
          output.status = apksx::WorkerStatus::BadOutput;
          output.message = L"解析输出结构不认识";
        }
      }
    }
    if (output.status == apksx::WorkerStatus::Ok) result->raw_json = output.stdout_text;
  }

  // 失败的文案：页面内一行字 + 复制诊断（没有弹窗）
  if (result->ok) {
    wchar_t buf[96];
    if (want_hash) {
      _snwprintf_s(buf, _TRUNCATE, L"哈希已计算（%llu ms）", elapsed);
    } else {
      _snwprintf_s(buf, _TRUNCATE, L"解析完成 · %llu ms", elapsed);
    }
    result->footer = buf;
    if (!want_hash) {
      int icon_px = 64;
      if (const HWND window = state->Window(); window != nullptr) {
        const UINT dpi = ::GetDpiForWindow(window);
        if (dpi != 0) icon_px = ::MulDiv(46, static_cast<int>(dpi), 96);
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
      if (result->icon != nullptr) ::DeleteObject(result->icon);  // 页面已关
      return;
    }
    state->pending = std::move(result);
    ::PostMessageW(state->hwnd, want_hash ? kMsgHashDone : kMsgInfoDone, 0, 0);
  }
}

void StartWorker(HWND hwnd, const std::shared_ptr<PageState>& state, bool want_hash) {
  state->started_ms = ::GetTickCount64();
  state->cancel.store(false);
  if (want_hash) {
    state->hash_running = true;
    state->footer = L"正在计算 SHA-256…";
  } else {
    state->worker_running = true;
    state->info_ok = false;
    state->footer = L"正在解析…";
  }
  Relayout(hwnd, state.get());
  ::InvalidateRect(hwnd, nullptr, TRUE);

  // 线程持有一份 shared_ptr：即使 shell 先释放了 PropPage，PageState 也活到线程退出
  std::thread worker(WorkerThread, state, want_hash, state->path);
  worker.detach();
}

// ------------------------------------------------------------------ 布局 / 滚动

bool NeedHashButton(const PageState* state) {
  if (state->hash_running || !state->info_ok) return false;
  const apksx::PageRow* row = apksx::FindRow(state->model, L"SHA-256");
  return row != nullptr && row->action;
}

bool NeedRetryButton(const PageState* state) {
  return !state->worker_running && !state->info_ok;
}

void UpdateScrollbar(HWND hwnd, PageState* state) {
  RECT client{};
  ::GetClientRect(hwnd, &client);
  const int page_height = static_cast<int>(state->view.metrics().body.bottom -
                                           state->view.metrics().body.top);
  const int max_pos = std::max(0, state->view.metrics().content_height - 1);
  SCROLLINFO info{};
  info.cbSize = sizeof(info);
  info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
  info.nMin = 0;
  info.nMax = max_pos;
  info.nPage = static_cast<UINT>(std::max(0, page_height));
  info.nPos = state->scroll_y;
  ::SetScrollInfo(hwnd, SB_VERT, &info, TRUE);
  (void)client;
}

void Relayout(HWND hwnd, PageState* state) {
  HDC dc = ::GetDC(hwnd);
  if (dc != nullptr) {
    state->view.Layout(hwnd, dc, state->model, state->theme, NeedHashButton(state),
                       NeedRetryButton(state));
    ::ReleaseDC(hwnd, dc);
  }

  // 滚到底之后内容变短（例如展开项收起）：把滚动位置收回来
  const int page_height = static_cast<int>(state->view.metrics().body.bottom -
                                           state->view.metrics().body.top);
  const int max_scroll = std::max(0, state->view.metrics().content_height - page_height);
  state->scroll_y = std::clamp(state->scroll_y, 0, max_scroll);
  UpdateScrollbar(hwnd, state);
}

void ScrollBy(HWND hwnd, PageState* state, int delta) {
  const int page_height = static_cast<int>(state->view.metrics().body.bottom -
                                           state->view.metrics().body.top);
  const int max_scroll = std::max(0, state->view.metrics().content_height - page_height);
  const int next = std::clamp(state->scroll_y + delta, 0, max_scroll);
  if (next == state->scroll_y) return;
  state->scroll_y = next;
  UpdateScrollbar(hwnd, state);
  ::InvalidateRect(hwnd, nullptr, TRUE);
}

// ------------------------------------------------------------------ 交互

void StartHashIfPossible(HWND hwnd, PageState* state) {
  if (!NeedHashButton(state)) return;
  if (state->worker_running || state->hash_running) return;
  StartWorker(hwnd, state->shared_from_this(), true);
}

void CopyAllJson(HWND hwnd, PageState* state) {
  std::wstring wide;
  if (!state->raw_json.empty() && apksx::Utf8ToWide(state->raw_json, &wide)) {
    CopyTextToClipboard(hwnd, wide);
    state->footer = L"已复制 JSON 到剪贴板";
  } else {
    state->footer = L"还没有可复制的 JSON";
  }
  ::InvalidateRect(hwnd, nullptr, TRUE);
}

void CopyItem(HWND hwnd, PageState* state, int index, bool with_key) {
  if (index < 0) return;
  const apksx::HitResult ref = state->view.RefAt(index);
  std::wstring text;
  if (ref.kind == apksx::HitKind::Row && ref.group < state->model.groups.size() &&
      ref.row < state->model.groups[ref.group].rows.size()) {
    const apksx::PageRow& row = state->model.groups[ref.group].rows[ref.row];
    text = with_key && !row.key.empty() ? row.key + L": " + row.value : row.value;
    if (text.empty()) text = row.copy_value;
  } else {
    text = state->view.ItemText(ref, state->model);
  }
  if (text.empty()) return;
  if (CopyTextToClipboard(hwnd, text)) {
    state->footer = L"已复制到剪贴板";
    ::InvalidateRect(hwnd, nullptr, TRUE);
  }
}

void ToggleRow(HWND hwnd, PageState* state, int index) {
  const apksx::HitResult ref = state->view.RefAt(index);
  if (ref.kind != apksx::HitKind::Row) return;
  if (ref.group >= state->model.groups.size()) return;
  apksx::PageGroup& group = state->model.groups[ref.group];
  if (ref.row >= group.rows.size()) return;
  apksx::PageRow& row = group.rows[ref.row];
  if (!row.expandable()) return;
  row.expanded = !row.expanded;
  Relayout(hwnd, state);
  ::InvalidateRect(hwnd, nullptr, TRUE);
}

void OnClick(HWND hwnd, PageState* state, int x, int y) {
  const apksx::HitResult hit = state->view.HitTest(x, y, state->scroll_y);
  if (hit.kind == apksx::HitKind::Button) {
    if (hit.button == apksx::PageView::kButtonHash) {
      StartHashIfPossible(hwnd, state);
    } else if (hit.button == apksx::PageView::kButtonCopyJson) {
      CopyAllJson(hwnd, state);
    } else if (hit.button == apksx::PageView::kButtonRetry) {
      StartWorker(hwnd, state->shared_from_this(), false);
    }
    return;
  }
  const int index = state->view.HitIndex(x, y, state->scroll_y);
  if (index < 0) return;
  state->selected = index;
  const apksx::HitResult ref = state->view.RefAt(index);
  if (ref.kind == apksx::HitKind::Row) {
    if (ref.group < state->model.groups.size() &&
        ref.row < state->model.groups[ref.group].rows.size()) {
      const apksx::PageRow& row = state->model.groups[ref.group].rows[ref.row];
      if (row.key == L"SHA-256" && row.action) {
        StartHashIfPossible(hwnd, state);
      } else if (row.expandable()) {
        ToggleRow(hwnd, state, index);
      }
    }
  }
  ::InvalidateRect(hwnd, nullptr, TRUE);
}

void ShowContextMenu(HWND hwnd, PageState* state, int x, int y) {
  const int index = state->view.HitIndex(x, y, state->scroll_y);
  state->selected = index;
  ::InvalidateRect(hwnd, nullptr, TRUE);
  if (index < 0) return;

  HMENU menu = ::CreatePopupMenu();
  if (menu == nullptr) return;
  ::AppendMenuW(menu, MF_STRING, kMenuCopyValue, L"复制值");
  ::AppendMenuW(menu, MF_STRING, kMenuCopyPair, L"复制 \u201C键: 值\u201D");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kMenuCopyJson, L"复制全部为 JSON");

  POINT pt{x, y};
  ::ClientToScreen(hwnd, &pt);
  const int command =
      static_cast<int>(::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd,
                                        nullptr));
  ::DestroyMenu(menu);

  switch (command) {
    case kMenuCopyValue: CopyItem(hwnd, state, index, false); break;
    case kMenuCopyPair: CopyItem(hwnd, state, index, true); break;
    case kMenuCopyJson: CopyAllJson(hwnd, state); break;
    default: break;
  }
}

void OnKeyDown(HWND hwnd, PageState* state, int vk) {
  const int count = state->view.ItemCount();
  switch (vk) {
    case VK_DOWN:
    case VK_UP: {
      if (count == 0) return;
      int index = state->selected;
      const int step = (vk == VK_DOWN) ? 1 : -1;
      for (int guard = 0; guard < count; ++guard) {
        index += step;
        if (index < 0) index = 0;
        if (index >= count) index = count - 1;
        const apksx::HitResult ref = state->view.RefAt(index);
        if (ref.kind == apksx::HitKind::Row || ref.kind == apksx::HitKind::Child) {
          state->selected = index;
          break;
        }
      }
      const int top = state->view.ItemTop(state->selected);
      const int height = state->view.ItemHeight(state->selected);
      const int page_height = static_cast<int>(state->view.metrics().body.bottom -
                                               state->view.metrics().body.top);
      if (top - state->scroll_y < 0) {
        ScrollBy(hwnd, state, top - state->scroll_y);
      } else if (top + height - state->scroll_y > page_height) {
        ScrollBy(hwnd, state, top + height - page_height - state->scroll_y);
      }
      ::InvalidateRect(hwnd, nullptr, TRUE);
      break;
    }
    case VK_HOME: ScrollBy(hwnd, state, -state->scroll_y); break;
    case VK_END: {
      const int page_height = static_cast<int>(state->view.metrics().body.bottom -
                                               state->view.metrics().body.top);
      ScrollBy(hwnd, state, state->view.metrics().content_height - page_height - state->scroll_y);
      break;
    }
    case 'C':
      if (::GetKeyState(VK_CONTROL) < 0) CopyItem(hwnd, state, state->selected, false);
      break;
    case 'A':
      if (::GetKeyState(VK_CONTROL) < 0) CopyAllJson(hwnd, state);
      break;
    case VK_RETURN:
    case VK_SPACE:
      if (state->selected >= 0) {
        const apksx::HitResult ref = state->view.RefAt(state->selected);
        if (ref.kind == apksx::HitKind::Row && ref.group < state->model.groups.size() &&
            ref.row < state->model.groups[ref.group].rows.size()) {
          const apksx::PageRow& row = state->model.groups[ref.group].rows[ref.row];
          if (row.key == L"SHA-256" && row.action) {
            StartHashIfPossible(hwnd, state);
          } else if (row.expandable()) {
            ToggleRow(hwnd, state, state->selected);
          }
        }
      }
      break;
    default: break;
  }
}

void HideTooltip(PageState* state) {
  if (state->tooltip == nullptr || !state->tooltip_visible) return;
  TOOLINFOW info{};
  info.cbSize = sizeof(info);
  info.hwnd = state->hwnd;
  info.uId = 1;
  ::SendMessageW(state->tooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&info));
  state->tooltip_visible = false;
}

void PositionTooltip(PageState* state, int x, int y) {
  if (state->tooltip == nullptr || !state->tooltip_visible) return;
  POINT pt{x, y};
  ::ClientToScreen(state->hwnd, &pt);
  pt.x += 16;
  pt.y += 20;
  ::SendMessageW(state->tooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x, pt.y));
}

// 只有"悬停停留够久 + 内容可能被省略号截断"才显示
void ShowTooltip(HWND hwnd, PageState* state) {
  if (state->tooltip == nullptr || state->hot < 0) return;
  // 只在"上一帧确实被省略号截断"时才出 tooltip（原生行为），不再按字符数猜
  if (!state->view.ItemTruncated(state->hot)) return;
  const apksx::HitResult ref = state->view.RefAt(state->hot);
  std::wstring text = state->view.ItemText(ref, state->model);
  if (text.empty()) return;
  TOOLINFOW info{};
  info.cbSize = sizeof(info);
  info.hwnd = hwnd;
  info.uId = 1;
  info.lpszText = text.data();
  ::SendMessageW(state->tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
  POINT pt{state->last_mouse_x, state->last_mouse_y};
  ::ClientToScreen(hwnd, &pt);
  pt.x += 16;
  pt.y += 20;
  ::SendMessageW(state->tooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x, pt.y));
  ::SendMessageW(state->tooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&info));
  state->tooltip_visible = true;
}

// 只重画某个 item（hover/选中变化时用；比整窗重画便宜两个数量级）
void InvalidateItem(HWND hwnd, PageState* state, int index) {
  RECT rc{};
  if (state->view.ItemClientRect(index, state->scroll_y, &rc)) {
    ::InvalidateRect(hwnd, &rc, FALSE);
  }
}

void EnsureBuffer(PageState* state, HDC reference, int width, int height) {
  if (width <= 0 || height <= 0) return;
  if (state->buffer_dc != nullptr && state->buffer_width == width &&
      state->buffer_height == height) {
    return;
  }
  if (state->buffer_dc != nullptr) {
    ::DeleteDC(state->buffer_dc);
    state->buffer_dc = nullptr;
  }
  if (state->buffer_bitmap != nullptr) {
    ::DeleteObject(state->buffer_bitmap);
    state->buffer_bitmap = nullptr;
  }
  state->buffer_dc = ::CreateCompatibleDC(reference);
  if (state->buffer_dc == nullptr) return;
  state->buffer_bitmap = ::CreateCompatibleBitmap(reference, width, height);
  if (state->buffer_bitmap == nullptr) {
    ::DeleteDC(state->buffer_dc);
    state->buffer_dc = nullptr;
    return;
  }
  ::SelectObject(state->buffer_dc, state->buffer_bitmap);
  state->buffer_width = width;
  state->buffer_height = height;
}

void ReleaseBuffer(PageState* state) {
  if (state->buffer_dc != nullptr) {
    ::DeleteDC(state->buffer_dc);
    state->buffer_dc = nullptr;
  }
  if (state->buffer_bitmap != nullptr) {
    ::DeleteObject(state->buffer_bitmap);
    state->buffer_bitmap = nullptr;
  }
  state->buffer_width = 0;
  state->buffer_height = 0;
}

void CreateTooltip(HWND hwnd, PageState* state) {
  state->tooltip = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                     WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                                     CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr,
                                     g_module, nullptr);
  if (state->tooltip == nullptr) return;
  TOOLINFOW info{};
  info.cbSize = sizeof(info);
  info.uFlags = TTF_TRACK | TTF_ABSOLUTE;
  info.hwnd = hwnd;
  info.uId = 1;
  info.lpszText = const_cast<wchar_t*>(L"");
  ::SendMessageW(state->tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
  ::SendMessageW(state->tooltip, TTM_SETMAXTIPWIDTH, 0, static_cast<LPARAM>(420));
}

// ------------------------------------------------------------------ 对话框

void ApplyPending(HWND hwnd, PageState* state);

INT_PTR CALLBACK DlgProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

class PropPage final : public IShellPropSheetExt, public IPersistFile, public IShellExtInit {
 public:
  PropPage() { IncrementServerLock(); }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    apksx::DebugLogf(L"apkprop: QI riid=%08lX-%04X-%04X", static_cast<unsigned long>(riid.Data1),
                     static_cast<unsigned>(riid.Data2), static_cast<unsigned>(riid.Data3));
    if (riid == IID_IUnknown || riid == IID_IShellPropSheetExt) {
      *ppv = static_cast<IShellPropSheetExt*>(this);
    } else if (riid == IID_IPersistFile || riid == IID_IPersist) {
      *ppv = static_cast<IPersistFile*>(this);
    } else if (riid == IID_IShellExtInit) {
      // 实测：这个 Windows build 的属性对话框在建页之前先 QI IShellExtInit，
      // 拿不到就整个跳过 handler（IPersistFile::Load 都不会被调）。
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

  // IShellPropSheetExt
  HRESULT STDMETHODCALLTYPE AddPages(LPFNADDPROPSHEETPAGE add_page, LPARAM lparam) override {
    if (add_page == nullptr) return E_POINTER;
    try {
      apksx::DebugLogf(L"apkprop: AddPages path=%s", path_.c_str());
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
      apksx::DebugLogf(L"apkprop: CreatePropertySheetPage -> %p", handle);
      if (handle == nullptr) return E_FAIL;
      AddRef();  // WM_DESTROY 时 Release
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
    apksx::DebugLogf(L"apkprop: ReplacePage id=%u", page_id);
    (void)page_id;
    (void)add_page;
    (void)lparam;
    return E_NOTIMPL;
  }

  // IPersistFile
  HRESULT STDMETHODCALLTYPE GetClassID(CLSID* clsid) override {
    apksx::DebugLog(L"apkprop: GetClassID");
    if (clsid == nullptr) return E_POINTER;
    *clsid = CLSID_ApkPropertyPage;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE IsDirty() override { return S_FALSE; }

  HRESULT STDMETHODCALLTYPE Load(LPCOLESTR filename, DWORD mode) override {
    (void)mode;
    if (filename == nullptr) return E_INVALIDARG;
    path_ = filename;
    apksx::DebugLogf(L"apkprop: IPersistFile::Load %s", path_.c_str());
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

  // IShellExtInit：属性对话框与"发送到"等宿主用它把选中项递进来
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
    apksx::DebugLogf(L"apkprop: IShellExtInit::Initialize %s", path_.c_str());
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetCurFile(LPOLESTR* filename) override {
    if (filename == nullptr) return E_POINTER;
    *filename = nullptr;
    return S_FALSE;
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
  auto* page = reinterpret_cast<PropPage*>(::GetWindowLongPtrW(hwnd, DWLP_USER));
  PageState* state = nullptr;
  if (page != nullptr) state = page->state().get();

  switch (message) {
    case WM_INITDIALOG: {
      auto* psp = reinterpret_cast<PROPSHEETPAGEW*>(lparam);
      page = psp == nullptr ? nullptr : reinterpret_cast<PropPage*>(psp->lParam);
      if (page == nullptr) return FALSE;
      ::SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(page));
      state = page->state().get();
      if (state == nullptr) return FALSE;

      {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->hwnd = hwnd;
        state->cancel.store(false);
      }
      (void)apksx::LoadTheme(&state->theme, hwnd);
      CreateTooltip(hwnd, state);

      // 骨架：先给一个"正在解析…"的模型，首屏立刻有内容
      state->model = apksx::PageModel();
      state->model.label = state->file_name;
      state->footer = L"正在解析…";
      Relayout(hwnd, state);

      StartWorker(hwnd, page->state(), false);
      return TRUE;  // 让系统选默认焦点
    }

    case WM_SIZE: {
      if (state == nullptr) break;
      Relayout(hwnd, state);
      ::InvalidateRect(hwnd, nullptr, TRUE);
      break;
    }

    case WM_ERASEBKGND:
      return 1;  // 全部自绘

    case WM_PAINT: {
      if (state == nullptr) break;
      // 必须在 BeginPaint 之前取：BeginPaint 会把更新区域"验证"掉
      RECT update{};
      if (::GetUpdateRect(hwnd, &update, FALSE)) {
        apksx::DebugLogf(L"propsheet: paint invalid=%ldx%ld",
                         static_cast<long>(update.right - update.left),
                         static_cast<long>(update.bottom - update.top));
      }
      PAINTSTRUCT ps{};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (dc != nullptr) {
        RECT client{};
        ::GetClientRect(hwnd, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        // 双缓冲：直接画到窗口 DC 时，整窗重画能看到"先底、再行、再底栏"的中间态（快滑时闪）
        EnsureBuffer(state, dc, width, height);
        HDC target = state->buffer_dc != nullptr ? state->buffer_dc : dc;
        state->view.Paint(target, hwnd, state->model, state->theme, state->scroll_y, state->hot,
                          state->selected, state->footer, NeedHashButton(state), state->icon);
        if (target != dc) ::BitBlt(dc, 0, 0, width, height, target, 0, 0, SRCCOPY);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_APP + 1:
    case WM_APP + 2: {
      if (state == nullptr) break;
      ApplyPending(hwnd, state);
      break;
    }

    case WM_VSCROLL: {
      if (state == nullptr) break;
      SCROLLINFO info{};
      info.cbSize = sizeof(info);
      info.fMask = SIF_ALL;
      ::GetScrollInfo(hwnd, SB_VERT, &info);
      int target = state->scroll_y;
      switch (LOWORD(wparam)) {
        case SB_LINEUP: target -= state->theme.Scale(20); break;
        case SB_LINEDOWN: target += state->theme.Scale(20); break;
        case SB_PAGEUP: target -= static_cast<int>(info.nPage); break;
        case SB_PAGEDOWN: target += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: target = info.nTrackPos; break;
        case SB_TOP: target = 0; break;
        case SB_BOTTOM: target = info.nMax; break;
        default: break;
      }
      ScrollBy(hwnd, state, target - state->scroll_y);
      break;
    }

    case WM_MOUSEWHEEL: {
      if (state == nullptr) break;
      const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
      const int lines = -delta / WHEEL_DELTA * state->view.metrics().row_height * 3;
      ScrollBy(hwnd, state, lines);
      break;
    }

    case WM_MOUSEMOVE: {
      if (state == nullptr) break;
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      state->last_mouse_x = x;
      state->last_mouse_y = y;
      if (!state->tracking_mouse) {
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        ::TrackMouseEvent(&track);
        state->tracking_mouse = true;
      }

      const int index = state->view.HitIndex(x, y, state->scroll_y);
      if (index != state->hot) {
        // 脏矩形：只重画旧行与新行，整窗重画留给滚动/尺寸变化
        InvalidateItem(hwnd, state, state->hot);
        state->hot = index;
        InvalidateItem(hwnd, state, index);
        ::KillTimer(hwnd, kTooltipTimerId);
        HideTooltip(state);
        if (index >= 0) ::SetTimer(hwnd, kTooltipTimerId, kTooltipDwellMs, nullptr);
      } else {
        PositionTooltip(state, x, y);
      }

      const apksx::HitResult ref = index < 0 ? apksx::HitResult{} : state->view.RefAt(index);
      bool hand = ref.kind == apksx::HitKind::Button;
      if (ref.kind == apksx::HitKind::Row && ref.group < state->model.groups.size() &&
          ref.row < state->model.groups[ref.group].rows.size()) {
        const apksx::PageRow& row = state->model.groups[ref.group].rows[ref.row];
        hand = hand || row.action || row.expandable();
      }
      if (hand != state->hand_cursor) {
        state->hand_cursor = hand;
        ::SetCursor(::LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
      }
      break;
    }

    case WM_MOUSELEAVE: {
      if (state == nullptr) break;
      state->tracking_mouse = false;
      ::KillTimer(hwnd, kTooltipTimerId);
      HideTooltip(state);
      InvalidateItem(hwnd, state, state->hot);
      state->hot = -1;
      break;
    }

    case WM_TIMER: {
      if (state == nullptr) break;
      if (wparam == kTooltipTimerId) {
        ::KillTimer(hwnd, kTooltipTimerId);
        ShowTooltip(hwnd, state);
      }
      break;
    }

    case WM_SETCURSOR:
      return TRUE;

    case WM_LBUTTONDOWN: {
      if (state == nullptr) break;
      ::SetFocus(hwnd);
      OnClick(hwnd, state, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      break;
    }

    case WM_CONTEXTMENU: {
      if (state == nullptr) break;
      const HWND source = reinterpret_cast<HWND>(wparam);
      if (source != hwnd) break;
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (pt.x == -1 && pt.y == -1) {
        RECT rc{};
        ::GetWindowRect(hwnd, &rc);
        pt.x = rc.left + state->theme.Scale(20);
        pt.y = rc.top + state->theme.Scale(20);
      } else {
        ::ScreenToClient(hwnd, &pt);
      }
      ShowContextMenu(hwnd, state, pt.x, pt.y);
      break;
    }

    case WM_KEYDOWN: {
      if (state == nullptr) break;
      OnKeyDown(hwnd, state, static_cast<int>(wparam));
      break;
    }

    case WM_GETDLGCODE: {
      // 让页面拿到方向键/Ctrl+C，而不是被属性对话框吃掉
      return DLGC_WANTARROWS | DLGC_WANTCHARS;
    }

    // 亮暗由系统 / 宿主决定：只在这几个"系统外观变了"的消息上重新问一遍宿主，
    // 不自己读主题开关（用户 2026-11-04 的要求）。
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
    case WM_DPICHANGED: {
      if (state == nullptr) break;
      apksx::DestroyTheme(&state->theme);
      (void)apksx::LoadTheme(&state->theme, hwnd);
      Relayout(hwnd, state);
      ::InvalidateRect(hwnd, nullptr, TRUE);
      break;
    }

    case WM_DESTROY: {
      if (page == nullptr) break;
      PageState* shared_state = page->state().get();
      if (shared_state != nullptr) {
        {
          std::lock_guard<std::mutex> guard(shared_state->mutex);
          shared_state->hwnd = nullptr;
        }
        shared_state->cancel.store(true);  // 线程 20 ms 内会看到；worker 在 Job 里被杀掉
        ::KillTimer(hwnd, kTooltipTimerId);
        ReleaseBuffer(shared_state);
        if (shared_state->tooltip != nullptr) {
          ::DestroyWindow(shared_state->tooltip);
          shared_state->tooltip = nullptr;
        }
        if (shared_state->icon != nullptr) {
          ::DeleteObject(shared_state->icon);
          shared_state->icon = nullptr;
        }
        apksx::DestroyTheme(&shared_state->theme);
      }
      ::SetWindowLongPtrW(hwnd, DWLP_USER, 0);
      page->Release();  // AddPages 里的 AddRef
      break;
    }

    default: break;
  }
  return FALSE;
}

void ApplyPending(HWND hwnd, PageState* state) {
  std::unique_ptr<PendingResult> result;
  {
    std::lock_guard<std::mutex> guard(state->mutex);
    result = std::move(state->pending);
  }
  if (result == nullptr) return;

  if (result->kind == PendingResult::Kind::Info) {
    state->worker_running = false;
    if (result->ok) state->raw_json = result->raw_json;
    state->info_ok = result->ok;
    if (result->ok) {
      state->model = std::move(result->model);
      state->selected = -1;
      state->hot = -1;
      state->scroll_y = 0;
      if (result->icon != nullptr) {
        if (state->icon != nullptr) ::DeleteObject(state->icon);
        state->icon = result->icon;
        result->icon = nullptr;
      }
    } else {
      std::vector<std::wstring> lines;
      lines.push_back(result->footer);
      lines.push_back(L"可以点「重试」再解析一次。");
      apksx::PageModel model;
      apksx::BuildErrorModel(state->file_name, lines, &model);
      state->model = std::move(model);
    }
  } else {
    state->hash_running = false;
    if (result->ok) apksx::SetSha256(&state->model, result->sha256);
  }
  state->footer = result->footer;

  Relayout(hwnd, state);
  ::InvalidateRect(hwnd, nullptr, TRUE);
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

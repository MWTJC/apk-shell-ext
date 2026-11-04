#include "propsheet/page_list.h"

#include "core/i18n.h"

#include <commctrl.h>
#include <shlwapi.h>  // DLLVERSIONINFO / DllGetVersion

#include <algorithm>
#include <cstring>

namespace apksx {
namespace {

int AddRow(HWND list, int group_id, int indent, const wchar_t* key, const wchar_t* value) {
  LVITEMW item{};
  item.mask = LVIF_TEXT | LVIF_INDENT;
  if (group_id > 0) item.mask |= LVIF_GROUPID;
  item.iItem = ListView_GetItemCount(list);
  item.iIndent = indent;
  item.iGroupId = group_id;
  item.pszText = const_cast<wchar_t*>(key);
  const int index = static_cast<int>(::SendMessageW(list, LVM_INSERTITEMW, 0,
                                                    reinterpret_cast<LPARAM>(&item)));
  if (index < 0) return -1;
  if (value != nullptr && value[0] != L'\0') {
    LVITEMW sub{};
    sub.iSubItem = 1;
    sub.pszText = const_cast<wchar_t*>(value);
    ::SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(index),
                   reinterpret_cast<LPARAM>(&sub));
  }
  return index;
}

// comctl32 是 v5（进程没有 v6 清单）还是 v6：只有 v6 才有分组带。
// 不用 LVM_ENABLEGROUPVIEW 的返回值判断——它在 v6 上也会返回 FALSE，会把列表搞成"分组模式但无组"（整表空白）。
int ComctlMajorVersion() {
  static int cached = -1;
  if (cached >= 0) return cached;
  cached = 0;
  HMODULE module = ::GetModuleHandleW(L"comctl32.dll");
  if (module == nullptr) module = ::LoadLibraryW(L"comctl32.dll");
  if (module != nullptr) {
    auto get_version = reinterpret_cast<HRESULT(WINAPI*)(DLLVERSIONINFO*)>(
        ::GetProcAddress(module, "DllGetVersion"));
    if (get_version != nullptr) {
      DLLVERSIONINFO info{};
      info.cbSize = sizeof(info);
      if (SUCCEEDED(get_version(&info))) cached = static_cast<int>(info.dwMajorVersion);
    }
  }
  return cached;
}

void AddGroup(HWND list, int group_id, const std::wstring& title, bool collapsible) {
  LVGROUP group{};
  group.cbSize = sizeof(group);
  group.mask = LVGF_HEADER | LVGF_GROUPID;
  if (collapsible) {
    // 默认收起：权限/特性的明细很长，先给"有几项"的概览，需要时点箭头展开。
    // 箭头由系统画、点击也由系统处理，我们不做命中测试。
    group.mask |= LVGF_STATE;
    group.stateMask = LVGS_COLLAPSIBLE | LVGS_COLLAPSED;
    group.state = LVGS_COLLAPSIBLE | LVGS_COLLAPSED;
  }
  group.pszHeader = const_cast<wchar_t*>(title.c_str());
  group.cchHeader = static_cast<int>(title.size());
  group.iGroupId = group_id;
  ListView_InsertGroup(list, -1, &group);
}

}  // namespace

void ConfigureListView(HWND list) {
  ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP |
                                             LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER);
  // 分组视图在 PopulateListView 里按 comctl32 版本决定开关
}

void ApplyListColumns(HWND list, int width, int dpi) {
  // 用列表自身的客户区宽度算列宽，并预留竖直滚动条与 2px 边：否则会出现多余的横向滚动条
  int client_width = width;
  RECT client{};
  if (::GetClientRect(list, &client)) {
    client_width = std::max(0, static_cast<int>(client.right - client.left));
  }
  const int key_width = ::MulDiv(96, dpi, 96);
  const int value_width =
      std::max(120, client_width - key_width - ::GetSystemMetrics(SM_CXVSCROLL) - 2);
  ListView_SetColumnWidth(list, 0, key_width);
  ListView_SetColumnWidth(list, 1, value_width);
}

void PopulateListView(HWND list, const PageModel& model) {
  ::SendMessageW(list, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(list);
  ListView_RemoveAllGroups(list);
  // comctl32 v5 的宿主（没有 v6 清单的进程）不画分组带：此时把组标题降级成普通行，
  // 否则"权限/特性"这种只有值的明细会失去上下文。
  const bool groups_supported = ComctlMajorVersion() >= 6;
  ListView_EnableGroupView(list, groups_supported ? TRUE : FALSE);

  int group_id = 1;
  for (const PageGroup& group : model.groups) {
    if (!groups_supported) {
      AddRow(list, 0, 0, L"", group.title.c_str());
      for (const PageRow& row : group.rows) {
        AddRow(list, 0, 0, row.key.c_str(), row.value.c_str());
      }
      continue;
    }
    AddGroup(list, group_id, group.title, group.collapsible);
    for (const PageRow& row : group.rows) {
      // 子项缩进一级（系统画缩进），与"权限/特性"这类明细共存
      AddRow(list, group_id, 0, row.key.c_str(), row.value.c_str());
      for (const std::wstring& child : row.children) {
        AddRow(list, group_id, 1, L"", child.c_str());
      }
    }
    ++group_id;
  }
  if (groups_supported && !model.warnings.empty()) {
    AddGroup(list, group_id, Tr(Str::DiagGroup), false);
    for (const std::wstring& warning : model.warnings) {
      AddRow(list, group_id, 0, L"", warning.c_str());
    }
  }

  ::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
  ::InvalidateRect(list, nullptr, TRUE);
}

int SelectedListIndex(HWND list) {
  return ListView_GetNextItem(list, -1, LVNI_SELECTED);
}

std::wstring ListItemText(HWND list, int item, int subitem) {
  if (item < 0) return std::wstring();
  wchar_t buffer[2048] = {};
  LVITEMW query{};
  query.iSubItem = subitem;
  query.pszText = buffer;
  query.cchTextMax = static_cast<int>(std::size(buffer));
  ::SendMessageW(list, LVM_GETITEMTEXTW, static_cast<WPARAM>(item),
                 reinterpret_cast<LPARAM>(&query));
  return std::wstring(buffer);
}

int FindListRowByKey(HWND list, const std::wstring& key) {
  const int count = ListView_GetItemCount(list);
  for (int index = 0; index < count; ++index) {
    if (ListItemText(list, index, 0) == key) return index;
  }
  return -1;
}

void ScrollListRowIntoView(HWND list, int index) {
  if (index < 0) return;
  ListView_EnsureVisible(list, index, FALSE);
  ListView_SetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED,
                        LVIS_SELECTED | LVIS_FOCUSED);
}

}  // namespace apksx

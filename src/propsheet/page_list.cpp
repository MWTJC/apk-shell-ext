#include "propsheet/page_list.h"

#include <commctrl.h>

#include <algorithm>

namespace apksx {
namespace {

int AddRow(HWND list, int group_id, int indent, const wchar_t* key, const wchar_t* value) {
  LVITEMW item{};
  item.mask = LVIF_TEXT | LVIF_GROUPID | LVIF_INDENT;
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

void AddGroup(HWND list, int group_id, const std::wstring& title) {
  LVGROUP group{};
  group.cbSize = sizeof(group);
  group.mask = LVGF_HEADER | LVGF_GROUPID;
  group.pszHeader = const_cast<wchar_t*>(title.c_str());
  group.cchHeader = static_cast<int>(title.size());
  group.iGroupId = group_id;
  ListView_InsertGroup(list, -1, &group);
}

}  // namespace

void ConfigureListView(HWND list) {
  ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP |
                                             LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER);
  ListView_EnableGroupView(list, TRUE);
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
  ListView_EnableGroupView(list, TRUE);

  int group_id = 1;
  for (const PageGroup& group : model.groups) {
    AddGroup(list, group_id, group.title);
    for (const PageRow& row : group.rows) {
      // 子项缩进一级（系统画缩进），与"权限/特性"这类明细共存
      AddRow(list, group_id, 0, row.key.c_str(), row.value.c_str());
      for (const std::wstring& child : row.children) {
        AddRow(list, group_id, 1, L"", child.c_str());
      }
    }
    ++group_id;
  }
  if (!model.warnings.empty()) {
    AddGroup(list, group_id, L"诊断");
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

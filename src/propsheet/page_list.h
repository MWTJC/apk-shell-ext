// 用原生 ListView 呈现行模型：两列「属性 / 值」+ 系统分组标题（等同资源管理器"详细信息"页的形态）。
// 选中高亮、滚动、截断 tooltip、键盘、无障碍全部由系统控制——这里只做"塞数据"。
#pragma once

#include <windows.h>

#include <string>

#include "propsheet/page_model.h"

namespace apksx {

// 一次性配置样式（页面初始化时调用一次）
void ConfigureListView(HWND list);

// 列宽：键列固定 96 DIP（按 dpi 缩放），值列取剩余宽度
void ApplyListColumns(HWND list, int width, int dpi);

// 用模型重建全部组与行（先关重绘，结束时一次刷出）
void PopulateListView(HWND list, const PageModel& model);

int SelectedListIndex(HWND list);
// 找到第一列的文本等于 key 的行（用于"算完哈希后滚到那一行"）
int FindListRowByKey(HWND list, const std::wstring& key);
void ScrollListRowIntoView(HWND list, int index);
std::wstring ListItemText(HWND list, int item, int subitem);

}  // namespace apksx

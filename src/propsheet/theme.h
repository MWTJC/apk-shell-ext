// DPI 与字体。
//
// 页面用**原生控件**（STATIC / SysListView32 / BUTTON），配色、选中、滚动条、tooltip
// 都由系统/宿主自己画——我们不再持有任何颜色令牌（2026-11-04 用户决定：嵌入现有面板时自绘不是首选）。
// 这里只剩两件事：按 DPI 缩放尺寸，从宿主对话框拿字体。
#pragma once

#include <windows.h>

namespace apksx {

struct PageTheme {
  int dpi = 96;
  HFONT font = nullptr;
  HFONT font_bold = nullptr;

  int Scale(int dip) const { return ::MulDiv(dip, dpi, 96); }
};

// hwnd 是页面窗口；字体优先问宿主（WM_GETFONT），取不到才回落 SPI_GETNONCLIENTMETRICS
bool LoadTheme(PageTheme* out, HWND hwnd);
void DestroyTheme(PageTheme* theme);

}  // namespace apksx

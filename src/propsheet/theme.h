// 属性页的配色 / 字体 / DPI。
// 只用公开可读的信号：HKCU 的 AppsUseLightTheme 与 SPI_GETNONCLIENTMETRICS，
// 不调用未公开的 SetPreferredAppMode（Windows build 之间行为不一致，docs/m3-property-page.md §7）。
#pragma once

#include <windows.h>

namespace apksx {

struct PageTheme {
  int dpi = 96;
  COLORREF bg = RGB(255, 255, 255);
  COLORREF group_bg = RGB(247, 247, 247);
  COLORREF text = RGB(27, 27, 27);
  COLORREF muted = RGB(107, 107, 107);
  COLORREF separator = RGB(226, 226, 226);
  COLORREF accent = RGB(0, 103, 192);
  COLORREF selection = RGB(229, 241, 251);
  HFONT font = nullptr;
  HFONT font_bold = nullptr;

  int Scale(int dip) const { return ::MulDiv(dip, dpi, 96); }
};

// hwnd 用于取 DPI（GetDpiForWindow 失败时回落 96）
bool LoadTheme(PageTheme* out, HWND hwnd);
void DestroyTheme(PageTheme* theme);

// 供 WM_SETTINGCHANGE 时判断要不要重建主题
bool IsDarkModeEnabled();

}  // namespace apksx

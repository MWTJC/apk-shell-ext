// 属性页的字体 / DPI / 配色。
//
// 原则（用户 2026-11-04 明确）：**页内不决定亮暗与配色**——像 PySide6 用原生主题那样，
// 颜色和字体一律向"宿主的属性对话框"和"系统色"要，我们只决定排版。
// 这样属性窗口将来变成深色（系统或第三方主题）时，页内自动跟上；现在它是浅色，页内也是浅色。
//
// 取法（全部是公开 API）：
//   背景   SendMessage(host, WM_CTLCOLORDLG, hdc, host) 返回的 HBRUSH 的颜色
//   文本   SendMessage(host, WM_CTLCOLORSTATIC, hdc, page) 之后 GetTextColor(hdc)
//   字体   SendMessage(host, WM_GETFONT) 的 HFONT（拿不到才回落 SPI_GETNONCLIENTMETRICS）
//   其余   GetSysColor(COLOR_GRAYTEXT / COLOR_3DSHADOW / COLOR_HOTLIGHT / COLOR_HIGHLIGHT …)
#pragma once

#include <windows.h>

namespace apksx {

struct PageTheme {
  int dpi = 96;
  COLORREF bg = RGB(240, 240, 240);
  COLORREF text = RGB(0, 0, 0);
  COLORREF muted = RGB(109, 109, 109);
  COLORREF separator = RGB(160, 160, 160);
  COLORREF selection = RGB(0, 120, 215);
  COLORREF selected_text = RGB(255, 255, 255);
  COLORREF accent = RGB(0, 102, 204);
  HFONT font = nullptr;
  HFONT font_bold = nullptr;

  int Scale(int dip) const { return ::MulDiv(dip, dpi, 96); }
};

// hwnd 是页面自己的窗口；宿主从 GetParent(hwnd) 取
bool LoadTheme(PageTheme* out, HWND hwnd);
void DestroyTheme(PageTheme* theme);

// 按百分比把 a 混向 b（0 = 全是 a，100 = 全是 b）：hover 底色这类"系统色推导"用它
COLORREF BlendColor(COLORREF a, COLORREF b, int b_percent);

}  // namespace apksx

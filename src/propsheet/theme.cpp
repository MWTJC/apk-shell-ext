#include "propsheet/theme.h"

#include <string>

#include "core/log.h"

namespace apksx {
namespace {

COLORREF ColorFromBrush(HBRUSH brush, COLORREF fallback) {
  if (brush == nullptr) return fallback;
  LOGBRUSH info{};
  if (::GetObjectW(brush, sizeof(info), &info) != sizeof(info)) return fallback;
  if (info.lbStyle != BS_SOLID) return fallback;
  return info.lbColor;
}

// 宿主对话框的字体：属性表用的是 .rc 里的字体（通常就是 SPI 的消息字体），
// 但第三方宿主可能设成别的，所以优先问它。
HFONT CreateHostFont(HWND host, HWND page, int dpi, LONG weight) {
  LOGFONTW logfont{};
  bool have_logfont = false;

  if (host != nullptr) {
    auto font = reinterpret_cast<HFONT>(::SendMessageW(host, WM_GETFONT, 0, 0));
    if (font != nullptr && ::GetObjectW(font, sizeof(logfont), &logfont) == sizeof(logfont)) {
      have_logfont = true;
    }
  }
  if (!have_logfont) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
      logfont = metrics.lfMessageFont;
      have_logfont = true;
    }
  }
  if (!have_logfont) {
    return static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
  }
  if (dpi > 0) logfont.lfHeight = ::MulDiv(logfont.lfHeight, dpi, 96);
  logfont.lfWeight = weight;
  HFONT created = ::CreateFontIndirectW(&logfont);
  if (created == nullptr) return static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
  (void)page;
  return created;
}

}  // namespace

COLORREF BlendColor(COLORREF a, COLORREF b, int b_percent) {
  const int percent = b_percent < 0 ? 0 : (b_percent > 100 ? 100 : b_percent);
  const int keep = 100 - percent;
  const int r = (GetRValue(a) * keep + GetRValue(b) * percent) / 100;
  const int g = (GetGValue(a) * keep + GetGValue(b) * percent) / 100;
  const int bl = (GetBValue(a) * keep + GetBValue(b) * percent) / 100;
  return RGB(r, g, bl);
}

bool LoadTheme(PageTheme* out, HWND page) {
  if (out == nullptr) return false;
  PageTheme theme;

  UINT dpi = 96;
  if (page != nullptr) {
    dpi = ::GetDpiForWindow(page);
    if (dpi == 0) dpi = 96;
  }
  theme.dpi = static_cast<int>(dpi);

  HWND host = page == nullptr ? nullptr : ::GetParent(page);

  // 背景 / 文本：直接问宿主怎么画自己的对话框与静态控件。
  // 临时 DC 只用来让宿主的 WM_CTLCOLOR* 设置文字色并返回画刷。
  COLORREF background = ::GetSysColor(COLOR_BTNFACE);
  COLORREF text = ::GetSysColor(COLOR_WINDOWTEXT);
  HDC screen = ::GetDC(nullptr);
  HDC dc = screen == nullptr ? nullptr : ::CreateCompatibleDC(screen);
  if (dc != nullptr && host != nullptr) {
    auto brush = reinterpret_cast<HBRUSH>(
        ::SendMessageW(host, WM_CTLCOLORDLG, reinterpret_cast<WPARAM>(dc),
                       reinterpret_cast<LPARAM>(host)));
    background = ColorFromBrush(brush, background);

    ::SendMessageW(host, WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc),
                   reinterpret_cast<LPARAM>(page));
    const COLORREF host_text = ::GetTextColor(dc);
    if (host_text != CLR_INVALID) text = host_text;
  }
  if (dc != nullptr) ::DeleteDC(dc);
  if (screen != nullptr) ::ReleaseDC(nullptr, screen);

  theme.bg = background;
  theme.text = text;
  // 其余一律用系统色（它们本身就跟着系统主题走）
  theme.muted = ::GetSysColor(COLOR_GRAYTEXT);
  theme.separator = ::GetSysColor(COLOR_3DSHADOW);
  theme.accent = ::GetSysColor(COLOR_HOTLIGHT);
  theme.selection = ::GetSysColor(COLOR_HIGHLIGHT);
  theme.selected_text = ::GetSysColor(COLOR_HIGHLIGHTTEXT);

  theme.font = CreateHostFont(host, page, static_cast<int>(dpi), FW_NORMAL);
  theme.font_bold = CreateHostFont(host, page, static_cast<int>(dpi), FW_SEMIBOLD);
  *out = theme;
  DebugLogf(L"propsheet: theme dpi=%d bg=%06lX text=%06lX (from host)",
            static_cast<int>(dpi), static_cast<unsigned long>(background),
            static_cast<unsigned long>(text));
  return true;
}

void DestroyTheme(PageTheme* theme) {
  if (theme == nullptr) return;
  if (theme->font != nullptr) {
    ::DeleteObject(theme->font);
    theme->font = nullptr;
  }
  if (theme->font_bold != nullptr) {
    ::DeleteObject(theme->font_bold);
    theme->font_bold = nullptr;
  }
}

}  // namespace apksx

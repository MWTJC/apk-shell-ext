#include "propsheet/theme.h"

#include "core/log.h"

namespace apksx {
namespace {

HFONT CreateHostFont(HWND host, int dpi, LONG weight) {
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
  if (!have_logfont) return static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));

  if (dpi > 0) logfont.lfHeight = ::MulDiv(logfont.lfHeight, dpi, 96);
  logfont.lfWeight = weight;
  HFONT created = ::CreateFontIndirectW(&logfont);
  return created != nullptr ? created : static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
}

}  // namespace

bool LoadTheme(PageTheme* out, HWND page) {
  if (out == nullptr) return false;
  PageTheme theme;

  UINT dpi = 96;
  if (page != nullptr) {
    dpi = ::GetDpiForWindow(page);
    if (dpi == 0) dpi = 96;
  }
  theme.dpi = static_cast<int>(dpi);

  const HWND host = page == nullptr ? nullptr : ::GetParent(page);
  theme.font = CreateHostFont(host, theme.dpi, FW_NORMAL);
  theme.font_bold = CreateHostFont(host, theme.dpi, FW_SEMIBOLD);
  *out = theme;
  DebugLogf(L"propsheet: font dpi=%d", theme.dpi);
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

#include "propsheet/theme.h"

#include <string>

#include "core/log.h"

namespace apksx {
namespace {

bool ReadAppsUseLightTheme(bool* light) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD value = 1;
  DWORD size = sizeof(value);
  DWORD type = 0;
  const LONG status = ::RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                                         reinterpret_cast<LPBYTE>(&value), &size);
  ::RegCloseKey(key);
  if (status != ERROR_SUCCESS || type != REG_DWORD) return false;
  *light = value != 0;
  return true;
}

HFONT CreateMessageFont(HWND hwnd, int dpi, LONG weight) {
  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof(metrics);
  if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
    return static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
  }
  LOGFONTW logfont = metrics.lfMessageFont;
  if (dpi > 0) {
    logfont.lfHeight = ::MulDiv(logfont.lfHeight, dpi, 96);
  }
  logfont.lfWeight = weight;
  HFONT font = ::CreateFontIndirectW(&logfont);
  if (font == nullptr) return static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
  (void)hwnd;
  return font;
}

}  // namespace

bool IsDarkModeEnabled() {
  bool light = true;
  if (!ReadAppsUseLightTheme(&light)) return false;  // 读不到就当浅色（保守）
  return !light;
}

bool LoadTheme(PageTheme* out, HWND hwnd) {
  if (out == nullptr) return false;
  PageTheme theme;

  UINT dpi = 96;
  if (hwnd != nullptr) {
    dpi = ::GetDpiForWindow(hwnd);
    if (dpi == 0) dpi = 96;
  }
  theme.dpi = static_cast<int>(dpi);

  const bool dark = IsDarkModeEnabled();
  if (dark) {
    theme.bg = RGB(32, 32, 32);
    theme.group_bg = RGB(43, 43, 43);
    theme.text = RGB(240, 240, 240);
    theme.muted = RGB(160, 160, 160);
    theme.separator = RGB(58, 58, 58);
    theme.accent = RGB(76, 194, 255);
    theme.selection = RGB(45, 59, 74);
  }

  theme.font = CreateMessageFont(hwnd, static_cast<int>(dpi), FW_NORMAL);
  theme.font_bold = CreateMessageFont(hwnd, static_cast<int>(dpi), FW_SEMIBOLD);
  *out = theme;
  DebugLogf(L"propsheet: theme dpi=%d dark=%d", static_cast<int>(dpi), dark ? 1 : 0);
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

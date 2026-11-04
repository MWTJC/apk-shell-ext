#include "propsheet/page_layout.h"

#include <algorithm>

namespace apksx {
namespace {

int Scale(int dip, int dpi) { return ::MulDiv(dip, dpi, 96); }

RECT MakeRect(int left, int top, int width, int height) {
  RECT rect{};
  rect.left = left;
  rect.top = top;
  rect.right = left + std::max(0, width);
  rect.bottom = top + std::max(0, height);
  return rect;
}

}  // namespace

PageLayout ComputePageLayout(int dpi, int width, int height, bool show_hash, bool show_retry) {
  PageLayout out;
  if (dpi <= 0) dpi = 96;

  const int pad = Scale(12, dpi);
  const int icon_size = Scale(32, dpi);
  const int line_height = Scale(18, dpi);

  // 头部：图标 + 三行文字（应用名 / 包名 / 版本）
  const int header_top = pad;
  out.icon = MakeRect(pad, header_top, icon_size, icon_size);
  const int text_left = pad + icon_size + Scale(10, dpi);
  const int text_width = std::max(0, width - text_left - pad);
  out.label = MakeRect(text_left, header_top, text_width, Scale(20, dpi));
  out.package = MakeRect(text_left, header_top + Scale(20, dpi), text_width, line_height);
  out.version =
      MakeRect(text_left, header_top + Scale(20, dpi) + line_height, text_width, line_height);

  const int header_bottom = header_top + std::max(icon_size, Scale(20, dpi) + line_height * 2);
  out.separator = MakeRect(pad, header_bottom, std::max(0, width - pad * 2), 1);

  // 底栏：按钮从右往左摆，状态文字吃剩下的宽度
  const int footer_height = Scale(34, dpi);
  const int footer_top = height - footer_height;
  const int button_height = Scale(24, dpi);
  const int button_top = footer_top + (footer_height - button_height) / 2;
  int right = width - pad;

  const int copy_width = Scale(128, dpi);
  out.copy_button = MakeRect(right - copy_width, button_top, copy_width, button_height);
  right -= copy_width + Scale(6, dpi);

  const int hash_width = Scale(112, dpi);
  if (show_hash) {
    out.hash_button = MakeRect(right - hash_width, button_top, hash_width, button_height);
    right -= hash_width + Scale(6, dpi);
  }

  const int retry_width = Scale(64, dpi);
  if (show_retry) {
    out.retry_button = MakeRect(right - retry_width, button_top, retry_width, button_height);
    right -= retry_width + Scale(6, dpi);
  }

  out.status =
      MakeRect(pad, footer_top, std::max(0, right - pad - Scale(8, dpi)), footer_height);

  // 列表吃掉头部与底栏之间的所有空间
  const int list_top = header_bottom + Scale(8, dpi);
  const int list_bottom = footer_top - Scale(6, dpi);
  out.list = MakeRect(pad, list_top, std::max(0, width - pad * 2),
                      std::max(0, list_bottom - list_top));

  out.button_height = button_height;
  out.footer_top = footer_top;
  return out;
}

}  // namespace apksx

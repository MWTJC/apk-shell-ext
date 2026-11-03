#include "propsheet/page_view.h"

#include <algorithm>

namespace apksx {
namespace {

constexpr wchar_t kArrowCollapsed[] = L"\x25B8";  // ▸
constexpr wchar_t kArrowExpanded[] = L"\x25BE";   // ▾

void FillRectColor(HDC dc, const RECT& rc, COLORREF color) {
  HBRUSH brush = ::CreateSolidBrush(color);
  if (brush != nullptr) {
    ::FillRect(dc, &rc, brush);
    ::DeleteObject(brush);
  }
}

void DrawTextEllipsis(HDC dc, const std::wstring& text, RECT rc, UINT format) {
  if (text.empty() || rc.right <= rc.left) return;
  ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rc,
              format | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

int MeasureText(HDC dc, const std::wstring& text) {
  if (text.empty()) return 0;
  SIZE size{};
  if (!::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size)) return 0;
  return size.cx;
}

// 32bpp DIB（带 alpha）→ 目标 DC，做一次预乘 alpha 混合
void DrawIconAlpha(HDC dc, HBITMAP bitmap, int x, int y, int cx, int cy) {
  if (bitmap == nullptr) return;
  HDC mem = ::CreateCompatibleDC(dc);
  if (mem == nullptr) return;
  HGDIOBJ old = ::SelectObject(mem, bitmap);
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  ::SetStretchBltMode(dc, HALFTONE);
  ::AlphaBlend(dc, x, y, cx, cy, mem, 0, 0, cx, cy, blend);
  ::SelectObject(mem, old);
  ::DeleteDC(mem);
}

}  // namespace

void PageView::Layout(HWND hwnd, HDC dc, const PageModel& model, const PageTheme& theme,
                      bool show_hash, bool show_retry) {
  RECT client{};
  if (hwnd != nullptr) ::GetClientRect(hwnd, &client);
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;

  metrics_ = ViewMetrics();
  metrics_.padding = theme.Scale(12);
  metrics_.indent = theme.Scale(16);
  metrics_.header_height = theme.Scale(70);
  metrics_.footer_height = theme.Scale(34);
  metrics_.key_width = theme.Scale(78);

  int line = 16;
  if (dc != nullptr) {
    HGDIOBJ old = ::SelectObject(dc, theme.font);
    TEXTMETRICW metrics{};
    if (::GetTextMetricsW(dc, &metrics)) line = static_cast<int>(metrics.tmHeight);
    ::SelectObject(dc, old);
  }
  metrics_.row_height = line + theme.Scale(9);
  metrics_.group_height = line + theme.Scale(13);
  metrics_.child_height = line + theme.Scale(7);

  refs_.clear();
  rects_.clear();
  truncated_.clear();
  buttons_.clear();
  button_labels_.clear();
  button_ids_.clear();

  int y = 0;
  for (size_t g = 0; g < model.groups.size(); ++g) {
    const PageGroup& group = model.groups[g];
    refs_.push_back(ItemRef{ItemKind::GroupHeader, g, 0, 0});
    rects_.push_back(Rect{y, metrics_.group_height});
    y += metrics_.group_height;
    for (size_t r = 0; r < group.rows.size(); ++r) {
      refs_.push_back(ItemRef{ItemKind::Row, g, r, 0});
      rects_.push_back(Rect{y, metrics_.row_height});
      y += metrics_.row_height;
      const PageRow& row = group.rows[r];
      if (!row.expanded || row.children.empty()) continue;
      for (size_t c = 0; c < row.children.size(); ++c) {
        refs_.push_back(ItemRef{ItemKind::Child, g, r, c});
        rects_.push_back(Rect{y, metrics_.child_height});
        y += metrics_.child_height;
      }
    }
  }
  for (size_t w = 0; w < model.warnings.size(); ++w) {
    refs_.push_back(ItemRef{ItemKind::Warning, model.groups.size(), w, 0});
    rects_.push_back(Rect{y, metrics_.row_height});
    y += metrics_.row_height;
  }
  metrics_.content_height = y;
  truncated_.assign(refs_.size(), 0);

  metrics_.header = RECT{0, 0, width, metrics_.header_height};
  metrics_.footer = RECT{0, height - metrics_.footer_height, width, height};
  metrics_.body = RECT{0, metrics_.header_height, width, height - metrics_.footer_height};

  // 底栏按钮：从右往左排
  struct ButtonSpec {
    int id;
    const wchar_t* label;
    bool visible;
  };
  const ButtonSpec specs[] = {
      {kButtonCopyJson, L"复制全部为 JSON", true},
      {kButtonHash, L"计算 SHA-256", show_hash},
      {kButtonRetry, L"重试", show_retry},
  };
  int right = metrics_.footer.right - metrics_.padding;
  if (dc != nullptr) {
    HGDIOBJ old = ::SelectObject(dc, theme.font);
    for (const ButtonSpec& spec : specs) {
      if (!spec.visible) continue;
      const int text_width = MeasureText(dc, spec.label);
      const int button_width = text_width + theme.Scale(16);
      const int button_height = metrics_.footer_height - theme.Scale(8);
      RECT rc{right - button_width, metrics_.footer.top + theme.Scale(4), right,
              metrics_.footer.top + theme.Scale(4) + button_height};
      buttons_.push_back(rc);
      button_labels_.push_back(spec.label);
      button_ids_.push_back(spec.id);
      right -= button_width + theme.Scale(6);
    }
    ::SelectObject(dc, old);
  }
}

int PageView::ItemTop(int index) const {
  if (index < 0 || static_cast<size_t>(index) >= rects_.size()) return 0;
  return rects_[static_cast<size_t>(index)].top;
}

bool PageView::ItemTruncated(int index) const {
  if (index < 0 || static_cast<size_t>(index) >= truncated_.size()) return false;
  return truncated_[static_cast<size_t>(index)] != 0;
}

bool PageView::ItemClientRect(int index, int scroll_y, RECT* out) const {
  if (out == nullptr) return false;
  if (index < 0 || static_cast<size_t>(index) >= rects_.size()) return false;
  const Rect& layout = rects_[static_cast<size_t>(index)];
  const int top = metrics_.body.top + layout.top - scroll_y;
  RECT rc{metrics_.body.left, top, metrics_.body.right, top + layout.height};
  // 完全在可视区外就不需要重画
  if (rc.bottom <= metrics_.body.top || rc.top >= metrics_.body.bottom) return false;
  if (rc.top < metrics_.body.top) rc.top = metrics_.body.top;
  if (rc.bottom > metrics_.body.bottom) rc.bottom = metrics_.body.bottom;
  *out = rc;
  return true;
}

int PageView::ItemHeight(int index) const {
  if (index < 0 || static_cast<size_t>(index) >= rects_.size()) return 0;
  return rects_[static_cast<size_t>(index)].height;
}

void PageView::Paint(HDC dc, HWND hwnd, const PageModel& model, const PageTheme& theme,
                     int scroll_y, int hot, int selected, const std::wstring& footer_text,
                     bool show_hash_button, HBITMAP icon) const {
  RECT client{};
  if (hwnd != nullptr) ::GetClientRect(hwnd, &client);
  (void)show_hash_button;  // 按钮集合在 Layout 时已定

  ::SetBkMode(dc, TRANSPARENT);
  FillRectColor(dc, client, theme.bg);

  // ---------------------------------------------------------------- 头部
  {
    RECT header = metrics_.header;
    FillRectColor(dc, header, theme.bg);
    const int icon_size = theme.Scale(46);
    const int icon_x = header.left + metrics_.padding;
    const int icon_y = header.top + (metrics_.header_height - icon_size) / 2;
    if (icon != nullptr) {
      DrawIconAlpha(dc, icon, icon_x, icon_y, icon_size, icon_size);
    } else {
      RECT box{icon_x, icon_y, icon_x + icon_size, icon_y + icon_size};
      FillRectColor(dc, box, theme.bg);
      HBRUSH frame = ::CreateSolidBrush(theme.separator);
      if (frame != nullptr) {
        ::FrameRect(dc, &box, frame);
        ::DeleteObject(frame);
      }
      HGDIOBJ old = ::SelectObject(dc, theme.font_bold);
      ::SetTextColor(dc, theme.muted);
      RECT inner = box;
      ::DrawTextW(dc, L"APK", -1, &inner,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
      ::SelectObject(dc, old);
    }

    const int text_left = icon_x + icon_size + theme.Scale(12);
    int text_top = header.top + theme.Scale(10);
    HGDIOBJ old = ::SelectObject(dc, theme.font_bold);
    ::SetTextColor(dc, theme.text);
    RECT line_rc{text_left, text_top, header.right - metrics_.padding,
                 text_top + theme.Scale(22)};
    DrawTextEllipsis(dc, model.label, line_rc, DT_LEFT);
    text_top = line_rc.bottom + theme.Scale(2);

    ::SelectObject(dc, theme.font);
    ::SetTextColor(dc, theme.muted);
    if (!model.package.empty()) {
      RECT rc{text_left, text_top, header.right - metrics_.padding, text_top + theme.Scale(18)};
      DrawTextEllipsis(dc, model.package, rc, DT_LEFT);
      text_top = rc.bottom;
    }
    if (!model.version_line.empty()) {
      RECT rc{text_left, text_top, header.right - metrics_.padding, text_top + theme.Scale(18)};
      DrawTextEllipsis(dc, model.version_line, rc, DT_LEFT);
    }
    ::SelectObject(dc, old);

    HBRUSH below = ::CreateSolidBrush(theme.separator);
    if (below != nullptr) {
      RECT sep{header.left, header.bottom - 1, header.right, header.bottom};
      ::FillRect(dc, &sep, below);
      ::DeleteObject(below);
    }
  }

  // ---------------------------------------------------------------- 内容
  {
    ::SaveDC(dc);
    if (hwnd != nullptr) {
      ::SelectClipRgn(dc, nullptr);
      ::IntersectClipRect(dc, metrics_.body.left, metrics_.body.top, metrics_.body.right,
                          metrics_.body.bottom);
    }
    for (size_t i = 0; i < refs_.size(); ++i) {
      const ItemRef& ref = refs_[i];
      const Rect& layout = rects_[i];
      const int top = metrics_.body.top + layout.top - scroll_y;
      if (top + layout.height < metrics_.body.top || top > metrics_.body.bottom) continue;

      RECT row_rc{metrics_.body.left, top, metrics_.body.right, top + layout.height};
      const bool is_hot = static_cast<int>(i) == hot;
      const bool is_selected = static_cast<int>(i) == selected;

      switch (ref.kind) {
        case ItemKind::GroupHeader: {
          // 原生观感：不铺自定义底色，粗体标题 + 底部一条系统分隔线
          HGDIOBJ old = ::SelectObject(dc, theme.font_bold);
          ::SetTextColor(dc, theme.text);
          RECT text_rc = row_rc;
          text_rc.left += metrics_.padding;
          DrawTextEllipsis(dc, model.groups[ref.group].title, text_rc, DT_LEFT);
          ::SelectObject(dc, old);
          RECT line{row_rc.left + metrics_.padding, row_rc.bottom - 1,
                    row_rc.right - metrics_.padding, row_rc.bottom};
          FillRectColor(dc, line, theme.separator);
          break;
        }
        case ItemKind::Row: {
          if (is_selected) {
            FillRectColor(dc, row_rc, theme.selection);
          } else if (is_hot) {
            FillRectColor(dc, row_rc, BlendColor(theme.bg, theme.selection, 12));
          }
          const COLORREF key_color = is_selected ? theme.selected_text : theme.muted;
          const COLORREF value_color = is_selected ? theme.selected_text : theme.text;
          const PageRow& row = model.groups[ref.group].rows[ref.row];
          HGDIOBJ old = ::SelectObject(dc, theme.font);

          int key_left = metrics_.padding;
          if (row.expandable()) {
            RECT arrow{key_left, row_rc.top, key_left + theme.Scale(14), row_rc.bottom};
            ::SetTextColor(dc, key_color);
            DrawTextEllipsis(dc, row.expanded ? kArrowExpanded : kArrowCollapsed, arrow, DT_LEFT);
            key_left += theme.Scale(14);
          }
          RECT key_rc{key_left, row_rc.top, key_left + metrics_.key_width, row_rc.bottom};
          ::SetTextColor(dc, key_color);
          DrawTextEllipsis(dc, row.key, key_rc, DT_LEFT);

          // 键为空的行（错误说明 / 警告）从左边距开始画，不占键列
          const int value_left = row.key.empty() ? metrics_.padding
                                                 : key_left + metrics_.key_width + theme.Scale(8);
          RECT value_rc{value_left, row_rc.top, row_rc.right - metrics_.padding, row_rc.bottom};
          ::SetTextColor(dc, row.action ? (is_selected ? theme.selected_text : theme.accent)
                                        : value_color);
          if (!row.value.empty() && i < truncated_.size()) {
            const int available = value_rc.right - value_rc.left;
            truncated_[i] = static_cast<uint8_t>(
                MeasureText(dc, row.value) > available - theme.Scale(6) ? 1 : 0);
          }
          DrawTextEllipsis(dc, row.value, value_rc, DT_LEFT);
          if (row.action && is_hot) {
            // 动作行画一条下划线，表明可点
            SIZE size{};
            if (::GetTextExtentPoint32W(dc, row.value.c_str(), static_cast<int>(row.value.size()),
                                        &size)) {
              const int underline_y = row_rc.top + (row_rc.bottom - row_rc.top + size.cy) / 2;
              RECT line{value_rc.left, underline_y, value_rc.left + size.cx, underline_y + 1};
              FillRectColor(dc, line, theme.accent);
            }
          }
          ::SelectObject(dc, old);
          break;
        }
        case ItemKind::Child: {
          if (is_hot) FillRectColor(dc, row_rc, BlendColor(theme.bg, theme.selection, 12));
          const PageRow& row = model.groups[ref.group].rows[ref.row];
          if (ref.child < row.children.size()) {
            HGDIOBJ old = ::SelectObject(dc, theme.font);
            ::SetTextColor(dc, theme.muted);
            RECT text_rc = row_rc;
            text_rc.left += metrics_.padding + metrics_.indent + theme.Scale(14);
            if (i < truncated_.size()) {
              const int available = text_rc.right - text_rc.left;
              truncated_[i] = static_cast<uint8_t>(
                  MeasureText(dc, row.children[ref.child]) > available - theme.Scale(6) ? 1 : 0);
            }
            DrawTextEllipsis(dc, row.children[ref.child], text_rc, DT_LEFT);
            ::SelectObject(dc, old);
          }
          break;
        }
        case ItemKind::Warning: {
          if (is_hot) FillRectColor(dc, row_rc, BlendColor(theme.bg, theme.selection, 12));
          if (ref.row >= model.warnings.size()) break;
          HGDIOBJ old = ::SelectObject(dc, theme.font);
          ::SetTextColor(dc, theme.muted);
          RECT text_rc = row_rc;
          text_rc.left += metrics_.padding;
          DrawTextEllipsis(dc, L"\x26A0 " + model.warnings[ref.row], text_rc, DT_LEFT);
          ::SelectObject(dc, old);
          break;
        }
      }
    }
    ::RestoreDC(dc, -1);
  }

  // ---------------------------------------------------------------- 底栏
  {
    HBRUSH brush = ::CreateSolidBrush(theme.separator);
    if (brush != nullptr) {
      RECT sep{metrics_.footer.left, metrics_.footer.top, metrics_.footer.right,
               metrics_.footer.top + 1};
      ::FillRect(dc, &sep, brush);
      ::DeleteObject(brush);
    }
    FillRectColor(dc, metrics_.footer, theme.bg);

    HGDIOBJ old = ::SelectObject(dc, theme.font);
    ::SetTextColor(dc, theme.muted);
    RECT status_rc{metrics_.footer.left + metrics_.padding, metrics_.footer.top,
                   metrics_.footer.right - theme.Scale(200), metrics_.footer.bottom};
    DrawTextEllipsis(dc, footer_text, status_rc, DT_LEFT);

    for (size_t i = 0; i < buttons_.size(); ++i) {
      RECT rc = buttons_[i];
      ::SetTextColor(dc, theme.accent);
      ::DrawTextW(dc, button_labels_[i].c_str(), -1, &rc,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }
    ::SelectObject(dc, old);
  }
}

int PageView::HitIndex(int x, int y, int scroll_y) const {
  (void)x;
  if (y < metrics_.body.top || y >= metrics_.body.bottom) return -1;
  const int content_y = y - metrics_.body.top + scroll_y;
  for (size_t i = 0; i < rects_.size(); ++i) {
    const Rect& layout = rects_[i];
    if (content_y >= layout.top && content_y < layout.top + layout.height) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

HitResult PageView::RefAt(int index) const {
  HitResult hit;
  if (index < 0 || static_cast<size_t>(index) >= refs_.size()) return hit;
  const ItemRef& ref = refs_[static_cast<size_t>(index)];
  switch (ref.kind) {
    case ItemKind::GroupHeader:
    case ItemKind::Warning: hit.kind = HitKind::GroupHeader; break;
    case ItemKind::Row: hit.kind = HitKind::Row; break;
    case ItemKind::Child: hit.kind = HitKind::Child; break;
  }
  hit.group = ref.group;
  hit.row = ref.row;
  hit.child = ref.child;
  return hit;
}

HitResult PageView::HitTest(int x, int y, int scroll_y) const {
  HitResult hit;
  for (size_t i = 0; i < buttons_.size(); ++i) {
    const RECT& rc = buttons_[i];
    if (x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom) {
      hit.kind = HitKind::Button;
      hit.button = button_ids_[i];
      return hit;
    }
  }
  if (y < metrics_.body.top || y >= metrics_.body.bottom) return hit;

  const int content_y = y - metrics_.body.top + scroll_y;
  for (size_t i = 0; i < rects_.size(); ++i) {
    const Rect& layout = rects_[i];
    if (content_y < layout.top || content_y >= layout.top + layout.height) continue;
    const ItemRef& ref = refs_[i];
    switch (ref.kind) {
      case ItemKind::GroupHeader: hit.kind = HitKind::GroupHeader; break;
      case ItemKind::Row: hit.kind = HitKind::Row; break;
      case ItemKind::Child: hit.kind = HitKind::Child; break;
      case ItemKind::Warning: hit.kind = HitKind::GroupHeader; break;
    }
    hit.group = ref.group;
    hit.row = ref.row;
    hit.child = ref.child;
    return hit;
  }
  return hit;
}

std::wstring PageView::ItemText(const HitResult& hit, const PageModel& model) const {
  switch (hit.kind) {
    case HitKind::Row: {
      if (hit.group >= model.groups.size()) return std::wstring();
      const PageGroup& group = model.groups[hit.group];
      if (hit.row >= group.rows.size()) return std::wstring();
      const PageRow& row = group.rows[hit.row];
      if (row.key.empty()) return row.value;
      return row.key + L": " + row.value;
    }
    case HitKind::Child: {
      if (hit.group >= model.groups.size()) return std::wstring();
      const PageGroup& group = model.groups[hit.group];
      if (hit.row >= group.rows.size()) return std::wstring();
      const PageRow& row = group.rows[hit.row];
      if (hit.child >= row.children.size()) return std::wstring();
      return row.children[hit.child];
    }
    default: return std::wstring();
  }
}

}  // namespace apksx

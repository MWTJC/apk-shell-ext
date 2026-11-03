// 属性页的"布局 + 绘制"（不碰窗口消息，便于单独推理）。
// 全自绘：一行一个 item，组标题是条带，子项缩进；底栏画状态文字与按钮。
// 坐标系：items 用"内容坐标"（0 = 内容顶端，未减滚动），header/body/footer 用客户区坐标。
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "propsheet/page_model.h"
#include "propsheet/theme.h"

namespace apksx {

enum class HitKind { None, GroupHeader, Row, Child, Button };

struct HitResult {
  HitKind kind = HitKind::None;
  size_t group = 0;
  size_t row = 0;
  size_t child = 0;
  int button = -1;
};

struct ViewMetrics {
  int header_height = 0;
  int footer_height = 0;
  int content_height = 0;
  int row_height = 0;
  int group_height = 0;
  int child_height = 0;
  int key_width = 0;
  int indent = 0;
  int padding = 0;
  RECT header{};
  RECT body{};
  RECT footer{};
};

class PageView {
 public:
  // hwnd/dc 只用于度量文字；model 变了（或展开状态变了）就要重新 Layout
  void Layout(HWND hwnd, HDC dc, const PageModel& model, const PageTheme& theme, bool show_hash,
              bool show_retry);

  void Paint(HDC dc, HWND hwnd, const PageModel& model, const PageTheme& theme, int scroll_y,
             int hot, int selected, const std::wstring& footer_text, bool show_hash_button,
             HBITMAP icon) const;

  HitResult HitTest(int x, int y, int scroll_y) const;
  // 命中到第几个 item（-1 = 空白 / 底栏）；RefAt 把索引还原成 HitResult
  int HitIndex(int x, int y, int scroll_y) const;
  HitResult RefAt(int index) const;
  // 悬停/复制要用的完整文本（被省略号截断的那部分）
  std::wstring ItemText(const HitResult& hit, const PageModel& model) const;

  const ViewMetrics& metrics() const { return metrics_; }
  int ItemCount() const { return static_cast<int>(refs_.size()); }
  int ItemTop(int index) const;
  int ItemHeight(int index) const;
  // 某个 item 在"客户区坐标"里的矩形（已减滚动）；用于只重画变化的那几行
  bool ItemClientRect(int index, int scroll_y, RECT* out) const;
  // 上一帧这一行是否被省略号截断（tooltip 只在真的看不全时才出，不再靠字符数猜）
  bool ItemTruncated(int index) const;

  // 底栏按钮：0 = 计算 SHA-256，1 = 复制全部为 JSON，2 = 重试
  static constexpr int kButtonHash = 0;
  static constexpr int kButtonCopyJson = 1;
  static constexpr int kButtonRetry = 2;

 private:
  enum class ItemKind { GroupHeader, Row, Child, Warning };

  struct ItemRef {
    ItemKind kind = ItemKind::Row;
    size_t group = 0;
    size_t row = 0;
    size_t child = 0;
  };

  struct Rect {
    int top = 0;
    int height = 0;
  };

  std::vector<ItemRef> refs_;
  std::vector<Rect> rects_;
  std::vector<RECT> buttons_;
  std::vector<std::wstring> button_labels_;
  std::vector<int> button_ids_;
  mutable std::vector<uint8_t> truncated_;  // Paint 时填：文字宽 > 可用宽
  ViewMetrics metrics_;
};

}  // namespace apksx

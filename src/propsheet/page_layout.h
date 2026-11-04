// 属性页布局的**纯函数**版本。
//
// 为什么抽出来：M3 验收里"150%/200% DPI 下不裁切"原本只能人眼在真实缩放下看。
// 布局数学（图标尺寸、底栏按钮、列表区域、各处间距）全部是 dpi + 客户区尺寸的函数，
// 抽成纯函数后就能在没有 explorer、没有窗口的情况下对 DPI × 客户区尺寸做断言。
// prop_page.cpp 的 Relayout 只负责把这里算出来的矩形搬给 MoveWindow，不再自己算。
#pragma once

#include <windows.h>

namespace apksx {

// 所有 RECT 都是页面客户区坐标、单位是物理像素（已按 dpi 缩放）
struct PageLayout {
  RECT icon{};
  RECT label{};
  RECT package{};
  RECT version{};
  RECT separator{};
  RECT list{};
  RECT hash_button{};
  RECT copy_button{};
  RECT retry_button{};
  RECT status{};
  int button_height = 0;
  int footer_top = 0;
};

// show_hash / show_retry 决定底栏是否给这两个按钮留位（与控件的可见性一致）。
// 宽度不够时各矩形会被夹到 0（宽度不为负），但不会互相重叠。
PageLayout ComputePageLayout(int dpi, int width, int height, bool show_hash, bool show_retry);

}  // namespace apksx

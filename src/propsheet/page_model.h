// 属性页的"行模型"：把 `apkinfo --json` 的 schema v1 变成可绘制的分组行。
// 与 UI 无关（不 include windows 控件头），因此可以用 tests/propsheet_model_test.cpp 直接单测。
// 字段顺序见 docs/m3-property-page.md §2（v1 冻结，只增不删）。
#pragma once

#include <string>
#include <vector>

#include "core/common.h"
#include "core/json_lite.h"

namespace apksx {

struct PageRow {
  std::wstring key;
  std::wstring value;       // 绘制用（长值由绘制层省略号截断）
  std::wstring copy_value;  // 复制用（默认等于 value）
  std::vector<std::wstring> children;  // 可展开的子项（权限 / 特性）
  bool expanded = false;
  bool action = false;  // 可点击行（如"计算 SHA-256"）

  bool expandable() const { return !children.empty(); }
};

struct PageGroup {
  std::wstring title;
  std::vector<PageRow> rows;
};

struct PageModel {
  std::wstring label;         // 头部应用名（清单 label，缺省回落文件名）
  std::wstring package;
  std::wstring version_line;  // "1.2.3 (10203) · minSdk 26 / targetSdk 34"
  std::vector<PageGroup> groups;
  std::vector<std::wstring> warnings;
  int schema = 0;
};

// 正常路径：doc 是 apkinfo 的根对象；fallback_name 用于 label 缺失时回落
bool BuildPageModel(const json::Value& doc, const std::wstring& fallback_name, PageModel* out,
                    Error* err);

// 错误路径：标题 + 若干行说明（页内显示，不弹框）
void BuildErrorModel(const std::wstring& title, const std::vector<std::wstring>& lines,
                     PageModel* out);

// 按需哈希完成后回填（"文件与签名"组里的 SHA-256 行）
void SetSha256(PageModel* model, const std::wstring& hex);

// 供绘制层判断"这一行是动作行"（如计算哈希 / 重试）
const PageRow* FindRow(const PageModel& model, const std::wstring& key);

}  // namespace apksx

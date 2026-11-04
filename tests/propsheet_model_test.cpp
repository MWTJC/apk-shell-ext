// 属性页行模型的单测：不需要窗口、不需要 explorer、不碰 COM。
//
//   propsheet_model_test --selftest            内置样例（含坏 JSON），断言关键行
//   propsheet_model_test <apkinfo-json.json>   把 JSON 转成行模型并打印（人工对照用）
//
// 退出码 0 = 全部断言通过 / 1 = 有失败 / 2 = 用法错误
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/common.h"
#include "core/json_lite.h"
#include "propsheet/page_layout.h"
#include "propsheet/page_model.h"

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
  if (condition) {
    std::printf("  ok   %s\n", what);
  } else {
    std::printf("  FAIL %s\n", what);
    ++g_failures;
  }
}

const std::wstring* FindValue(const apksx::PageModel& model, const std::wstring& key) {
  for (const apksx::PageGroup& group : model.groups) {
    for (const apksx::PageRow& row : group.rows) {
      if (row.key == key) return &row.value;
    }
  }
  return nullptr;
}

const apksx::PageRow* FindRowAnywhere(const apksx::PageModel& model, const std::wstring& key) {
  for (const apksx::PageGroup& group : model.groups) {
    for (const apksx::PageRow& row : group.rows) {
      if (row.key == key) return &row;
    }
  }
  return nullptr;
}

// 与 apkinfo --json 形状一致的样例（字段裁剪到属性页用到的那些）
const char kSample[] = R"json({
  "schema": 1,
  "kind": "apk",
  "file": {"path": "D:\\x\\a.apk", "size": 8745, "mtime": 1793724730},
  "app": {"package": "com.example.pngicon", "label": "PNG Icon Demo",
          "versionName": "1.2.3", "versionCode": 10203, "minSdk": 26, "targetSdk": 34,
          "abis": ["arm64-v8a", "armeabi-v7a"],
          "densities": ["xxxhdpi", "xxhdpi"], "locales": ["zh-CN", "en"]},
  "icon": {"source": "res/mipmap-xxxhdpi-v4/ic_launcher.png", "width": 192, "height": 192,
           "adaptive": false},
  "signature": {"scheme": ["v1", "v2", "v3"],
                "certs": [{"subject": "CN=Android, O=Android, C=US",
                           "sha256": "a40da80a59d170caa950cf15c18c454d47a39b26989d8b640ecd745ba71bf5dc",
                           "notBefore": "2008-02-29T01:33:46Z",
                           "notAfter": "2035-07-17T01:33:46Z"}]},
  "permissions": [{"name": "android.permission.INTERNET", "declared": true},
                  {"name": "android.permission.CAMERA", "declared": true}],
  "features": [{"name": "android.hardware.camera", "required": true},
               {"name": "android.hardware.wifi", "required": false}],
  "components": {"activities": 12, "services": 3, "receivers": 2, "providers": 1},
  "diagnostics": {"warnings": ["未找到 launcher 图标"], "elapsedMs": 12.5}
})json";

// .xapk 形状（M4a）：container 块 + splits
const char kContainerSample[] = R"json({
  "schema": 1,
  "kind": "xapk",
  "file": {"path": "D:\\x\\p.xapk", "size": 22131352, "mtime": 1793773177},
  "app": {"package": "com.example.app", "label": "Example", "versionName": "1.0",
          "versionCode": 7, "minSdk": 21, "targetSdk": 34},
  "container": {"kind": "xapk", "base": "com.example.app.apk", "apkCount": 2,
                "splits": [{"name": "com.example.app.apk", "size": 15812534, "kind": "base", "base": true},
                           {"name": "config.armeabi_v7a.apk", "size": 6316540, "kind": "config"}]},
  "icon": {"source": "res/mipmap-xxxhdpi/app_icon.png", "width": 192, "height": 192,
           "adaptive": false},
  "components": {"activities": 1},
  "diagnostics": {"elapsedMs": 13.6}
})json";

void DumpModel(const apksx::PageModel& model) {
  std::wprintf(L"label: %ls\n", model.label.c_str());
  std::wprintf(L"package: %ls\n", model.package.c_str());
  std::wprintf(L"version: %ls\n", model.version_line.c_str());
  for (const apksx::PageGroup& group : model.groups) {
    std::wprintf(L"[%ls]\n", group.title.c_str());
    for (const apksx::PageRow& row : group.rows) {
      std::wprintf(L"  %ls = %ls%s\n", row.key.c_str(), row.value.c_str(),
                   row.action ? L"  (action)" : L"");
      for (const std::wstring& child : row.children) {
        std::wprintf(L"      - %ls\n", child.c_str());
      }
    }
  }
  for (const std::wstring& warning : model.warnings) {
    std::wprintf(L"! %ls\n", warning.c_str());
  }
}

const apksx::PageGroup* FindGroupByPrefix(const apksx::PageModel& model, const wchar_t* prefix) {
  for (const apksx::PageGroup& group : model.groups) {
    if (group.title.find(prefix) == 0) return &group;
  }
  return nullptr;
}

// ------------------------------------------------------------------ 布局 / DPI
// M3 验收里"150%/200% 下不裁切"的可复现版本：
// 布局是 (dpi, 客户区尺寸) 的纯函数，所以在没有 explorer、没有窗口的情况下也能断言。
// 客户区按 **DIP** 取网格（对话框随 DPI 等比变大，物理像素也等比变大），
// 覆盖 100% / 125% / 150% / 175% / 200% / 250% 六档缩放。
struct LayoutStats {
  int cases = 0;
  int outside = 0;
  int negative = 0;
  int overlap = 0;
  int header_overlap = 0;
  int list_overlap = 0;
  char first_failure[160] = {};
};

bool Inside(const RECT& rect, int width, int height) {
  return rect.left >= 0 && rect.top >= 0 && rect.right <= width && rect.bottom <= height;
}

bool Negative(const RECT& rect) { return rect.right < rect.left || rect.bottom < rect.top; }

// 零尺寸矩形（隐藏按钮）不算重叠
bool Overlaps(const RECT& a, const RECT& b) {
  if (a.right == a.left || a.bottom == a.top || b.right == b.left || b.bottom == b.top) return false;
  return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

LayoutStats SweepLayouts() {
  LayoutStats stats;
  const int dpis[] = {96, 120, 144, 168, 192, 240};  // 100% / 125% / 150% / 175% / 200% / 250%
  for (int dpi : dpis) {
    for (int width_dip = 360; width_dip <= 900; width_dip += 20) {
      for (int height_dip = 400; height_dip <= 1000; height_dip += 20) {
        for (int variant = 0; variant < 4; ++variant) {
          const bool show_hash = (variant & 1) != 0;
          const bool show_retry = (variant & 2) != 0;
          const int width = ::MulDiv(width_dip, dpi, 96);
          const int height = ::MulDiv(height_dip, dpi, 96);
          const apksx::PageLayout layout =
              apksx::ComputePageLayout(dpi, width, height, show_hash, show_retry);
          ++stats.cases;

          const RECT rects[] = {layout.icon,       layout.label,      layout.package,
                                layout.version,    layout.separator,  layout.list,
                                layout.hash_button, layout.copy_button, layout.retry_button,
                                layout.status};
          for (const RECT& rect : rects) {
            if (Negative(rect)) ++stats.negative;
            if (!Inside(rect, width, height)) {
              if (stats.outside == 0) {
                _snprintf_s(stats.first_failure, sizeof(stats.first_failure), _TRUNCATE,
                            "dpi=%d client=%dx%d variant=%d rect=%ld,%ld,%ld,%ld", dpi, width,
                            height, variant, static_cast<long>(rect.left),
                            static_cast<long>(rect.top), static_cast<long>(rect.right),
                            static_cast<long>(rect.bottom));
              }
              ++stats.outside;
            }
          }

          if (Overlaps(layout.copy_button, layout.hash_button) ||
              Overlaps(layout.copy_button, layout.retry_button) ||
              Overlaps(layout.hash_button, layout.retry_button) ||
              Overlaps(layout.status, layout.copy_button) ||
              Overlaps(layout.status, layout.hash_button) ||
              Overlaps(layout.status, layout.retry_button)) {
            ++stats.overlap;
          }

          // 头部三行文字不能压到图标
          if (layout.label.left < layout.icon.right || layout.package.left < layout.icon.right ||
              layout.version.left < layout.icon.right) {
            ++stats.header_overlap;
          }

          // 列表不能和头部/底栏叠在一起
          if (Overlaps(layout.list, layout.icon) || Overlaps(layout.list, layout.status)) {
            ++stats.list_overlap;
          }
        }
      }
    }
  }
  return stats;
}

void RunLayoutSelfTest() {
  std::printf("selftest: page layout / dpi\n");
  const LayoutStats stats = SweepLayouts();
  std::printf("  (swept %d layouts: dpi 96..240 x client 360..900 x 400..1000 DIP x 4 variants)\n",
              stats.cases);
  if (stats.outside != 0) std::printf("  first out-of-client: %s\n", stats.first_failure);
  Check(stats.outside == 0, "所有控件都在客户区内（无裁切）");
  Check(stats.negative == 0, "没有任何负宽高");
  Check(stats.overlap == 0, "底栏按钮/状态文字互不重叠");
  Check(stats.header_overlap == 0, "头部文字不压图标");
  Check(stats.list_overlap == 0, "列表不与头部/底栏重叠");

  // 100% 的黄金值：改动布局数学时这里会先炸，逼着人确认是有意改的
  const apksx::PageLayout base = apksx::ComputePageLayout(96, 468, 600, true, false);
  Check(base.icon.left == 12 && base.icon.top == 12 &&
            base.icon.right - base.icon.left == 32 && base.icon.bottom - base.icon.top == 32,
        "96 DPI 图标为 32px @ (12,12)");
  Check(base.list.left == 12 && base.list.right == 456,
        "96 DPI 列表左右各留 12px");
  Check(base.copy_button.right - base.copy_button.left == 128 &&
            base.hash_button.right - base.hash_button.left == 112,
        "96 DPI 按钮宽度 128 / 112");
  Check(base.button_height == 24 && base.footer_top == 600 - 34,
        "96 DPI 底栏高 34px、按钮高 24px");

  // 150% / 200%：几何必须是等比的（这也是"真实缩放下不会被裁"的前提）
  const apksx::PageLayout at150 = apksx::ComputePageLayout(144, 468 * 3 / 2, 600 * 3 / 2, true, false);
  const apksx::PageLayout at200 = apksx::ComputePageLayout(192, 468 * 2, 600 * 2, true, false);
  Check(at150.icon.right - at150.icon.left == 48 && at200.icon.right - at200.icon.left == 64,
        "150%/200% 图标按比例放大到 48px / 64px");
  Check(at150.list.left == 18 && at200.list.left == 24, "150%/200% 内边距按比例放大");
  Check(at200.list.right - at200.list.left == (base.list.right - base.list.left) * 2,
        "200% 列表宽度是 100% 的两倍");
}

int RunSelfTest() {
  std::printf("selftest: parse + model\n");
  apksx::json::ValuePtr doc;
  apksx::Error err;
  const bool parsed = apksx::json::Parse(kSample, &doc, &err);
  Check(parsed && doc != nullptr, "sample JSON parses");

  apksx::PageModel model;
  Check(apksx::BuildPageModel(*doc, L"a.apk", &model, &err), "BuildPageModel succeeds");

  Check(model.label == L"PNG Icon Demo", "header label from manifest");
  Check(model.package == L"com.example.pngicon", "header package");
  Check(model.version_line.find(L"1.2.3 (10203)") == 0, "header version line starts with name+code");
  Check(model.version_line.find(L"minSdk 26 / targetSdk 34") != std::wstring::npos,
        "header version line contains sdk");

  const std::wstring* package = FindValue(model, L"包名");
  Check(package != nullptr && *package == L"com.example.pngicon", "应用组有包名");

  const apksx::PageGroup* perms = FindGroupByPrefix(model, L"权限 ·");
  Check(perms != nullptr && perms->collapsible, "权限组可折叠");
  Check(perms != nullptr && perms->rows.size() == 2, "权限组有两条明细");
  Check(perms != nullptr && perms->title.find(L"2 项") != std::wstring::npos, "权限组标题带数量");
  Check(perms != nullptr && perms->rows[0].value.find(L"android.permission") == 0,
        "权限明细是权限名");

  const apksx::PageGroup* features = FindGroupByPrefix(model, L"特性 ·");
  Check(features != nullptr && features->collapsible, "特性组可折叠");
  Check(features != nullptr && features->rows.size() == 2, "特性组有两条明细");
  Check(features != nullptr && features->title.find(L"1 必需") != std::wstring::npos,
        "特性组标题统计 required");
  Check(features != nullptr &&
            features->rows[0].value.find(L"（必需）") != std::wstring::npos,
        "必需的特性带标注");
  Check(features != nullptr &&
            features->rows[1].value.find(L"（可选）") != std::wstring::npos,
        "可选的特性带标注");

  const apksx::PageRow* sha = FindRowAnywhere(model, L"SHA-256");
  Check(sha != nullptr && sha->action, "SHA-256 行默认是动作行（按需计算）");
  apksx::SetSha256(&model, L"deadbeef");
  sha = FindRowAnywhere(model, L"SHA-256");
  Check(sha != nullptr && !sha->action && sha->value == L"deadbeef", "SetSha256 回填并取消动作态");

  const std::wstring* schemes = FindValue(model, L"签名");
  Check(schemes != nullptr && *schemes == L"v1, v2, v3", "签名方案拼接");
  const std::wstring* digest = FindValue(model, L"证书摘要");
  Check(digest != nullptr && digest->size() == 64, "证书摘要为 64 位 hex");

  Check(model.warnings.size() == 1, "warnings 透传");

  // 没有签名 / 没有图标 / label 缺失的退化路径
  const char kMinimal[] = R"json({"schema":1,"file":{"size":1024},"app":{},"components":{}})json";
  apksx::json::ValuePtr minimal;
  if (apksx::json::Parse(kMinimal, &minimal, &err)) {
    apksx::PageModel minimal_model;
    Check(apksx::BuildPageModel(*minimal, L"fallback.apk", &minimal_model, &err),
          "minimal JSON builds");
    Check(minimal_model.label == L"fallback.apk", "label 回落文件名");
    const apksx::PageRow* sig = FindRowAnywhere(minimal_model, L"签名");
    Check(sig != nullptr && sig->value.find(L"未发现签名") != std::wstring::npos,
          "没有 signature 时明确写未签名");
    Check(FindValue(minimal_model, L"图标来源") == nullptr, "没有图标就没有图标行");
  } else {
    Check(false, "minimal JSON parses");
  }

  // ---- 容器（M4a）：xapk 的 container 块 → 「分包」分组 + 文件组两行
  {
    apksx::json::ValuePtr container_doc;
    Check(apksx::json::Parse(kContainerSample, &container_doc, &err) && container_doc != nullptr,
          "container JSON parses");
    apksx::PageModel container_model;
    Check(apksx::BuildPageModel(*container_doc, L"p.xapk", &container_model, &err),
          "container model builds");
    const apksx::PageGroup* splits = FindGroupByPrefix(container_model, L"分包 ·");
    Check(splits != nullptr && splits->collapsible, "分包组可折叠");
    Check(splits != nullptr && splits->rows.size() == 2, "分包组列出 base + config 两条");
    Check(splits != nullptr && splits->rows[0].value.find(L"[base]") == 0,
          "base 分包带 [base] 前缀");
    Check(splits != nullptr && splits->rows[1].value.find(L"[配置]") == 0,
          "config 分包带 [配置] 前缀");
    Check(splits != nullptr && splits->rows[1].value.find(L"6.0 MB") != std::wstring::npos,
          "分包行带大小");
    const std::wstring* container_kind = FindValue(container_model, L"安装包类型");
    Check(container_kind != nullptr && container_kind->find(L"XAPK") == 0,
          "文件组有「安装包类型 = XAPK（2 个 APK）」");
    const std::wstring* inner_apk = FindValue(container_model, L"内层 APK");
    Check(inner_apk != nullptr && *inner_apk == L"com.example.app.apk", "文件组有内层 APK 名");
  }

  // 坏 JSON：必须失败而不是"半个模型"
  apksx::json::ValuePtr broken;
  Check(!apksx::json::Parse("{\"a\": [1, 2,", &broken, &err), "truncated JSON rejected");
  Check(!apksx::json::Parse("{\"a\": 1} trailing", &broken, &err), "trailing garbage rejected");
  Check(apksx::json::Parse("{\"a\": \"\\u4e2d\\u6587\"}", &broken, &err), "unicode escape parsed");
  if (broken != nullptr) {
    const apksx::json::Value* a = broken->Find("a");
    std::wstring decoded;
    const bool ok = a != nullptr && apksx::Utf8ToWide(a->as_string(), &decoded);
    Check(ok && decoded == L"中文", "unicode escape decodes");
  }

  RunLayoutSelfTest();
  return g_failures == 0 ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::fwprintf(stderr, L"usage: propsheet_model_test --selftest|<json-file>\n");
    return 2;
  }
  const std::wstring first = argv[1];
  if (first == L"--selftest") return RunSelfTest();

  // 读文件（宽路径 → 窄路径交给 std::ifstream 太绕，这里用 Win32 直接读）
  HANDLE file = ::CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    std::fwprintf(stderr, L"cannot open %ls\n", argv[1]);
    return 2;
  }
  std::string text;
  char buffer[8192];
  DWORD got = 0;
  while (::ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
    text.append(buffer, got);
  }
  ::CloseHandle(file);

  apksx::json::ValuePtr doc;
  apksx::Error err;
  if (!apksx::json::Parse(text, &doc, &err) || doc == nullptr) {
    std::fwprintf(stderr, L"JSON parse failed\n");
    return 1;
  }
  std::wstring name = first;
  const size_t slash = name.find_last_of(L"\\/");
  if (slash != std::wstring::npos) name = name.substr(slash + 1);

  apksx::PageModel model;
  if (!apksx::BuildPageModel(*doc, name, &model, &err)) {
    std::fwprintf(stderr, L"BuildPageModel failed\n");
    return 1;
  }
  DumpModel(model);
  return 0;
}

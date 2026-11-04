// 鸿蒙 HAP（HarmonyOS Ability Package）：ZIP + `module.json`（JSON）+ `resources.index`（RestoolV2 私有二进制）。
//
// v1 能做什么 / 不能做什么（docs/formats.md §5）：
//   能：bundleName / versionName / versionCode / min–targetAPIVersion / 模块与设备类型 /
//       abilities 与 extensionAbilities 计数 / requestPermissions / 图标（$media: 启发式）；
//   不能：`$string:` 标签（要解 resources.index）、layered icon 的合成（只取 foreground 层）、
//       `$media:<资源id>`（用 JSON 的键名约定推断文件名）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/byte_source.h"
#include "core/common.h"

namespace apksx {

class ZipReader;
class Deadline;

struct HapInfo {
  std::string bundle_name;
  std::string version_name;
  uint32_t version_code = 0;

  std::string label;      // 字面量，或从 resources.index 解出来的真名
  std::string label_ref;  // 实际用来解的那条引用（含 $string: 前缀）
  uint32_t label_id = 0;  // 与 label_ref 对应的 labelId
  std::string app_label_ref;  // app.label 的引用（诊断用；桌面显示的是 ability 的 label）

  uint32_t min_api = 0;      // app.minAPIVersion（HarmonyOS 的 API 号，如 60000020）
  uint32_t target_api = 0;   // app.targetAPIVersion
  std::string compile_sdk;   // app.compileSdkVersion，如 "6.0.0.47"

  std::string module_name;  // module.name
  std::string module_type;  // module.type（entry / feature / shared）
  std::string main_element; // module.mainElement
  std::vector<std::string> device_types;

  uint32_t abilities = 0;
  uint32_t extension_abilities = 0;
  std::vector<std::string> permissions;  // module.requestPermissions[].name

  std::string icon_ref;  // app.icon，例如 "$media:layered_image"
  std::string start_icon_ref;  // 第一个带 startWindowIcon 的 ability（图标兜底）
};

// 根上有 module.json（且没有 AndroidManifest.xml）就当 HAP
bool LooksLikeHap(const ZipReader& zip);

bool ReadHapManifest(const ZipReader& zip, HapInfo* out, Error* err);

// 图标：$media: 名 → resources/base/media/<名>.png；layered_image.json 取 foreground；
// 都没命中时退到 app_icon/icon/最大 PNG。返回原始图像字节（不解码）。
struct HapIcon {
  std::vector<uint8_t> bytes;
  std::string path;  // HAP 内路径
  std::string note;
};

bool ReadHapIcon(const ZipReader& zip, const HapInfo& info, HapIcon* out, Error* err);

// 从 resources.index（RestoolV2）里取一个字符串资源的默认配置值。
// 只做属性页需要的那一件事：$string:name → 真名。结构见 restool 的 resource_table.h。
bool ResolveHapString(const std::vector<uint8_t>& index, uint32_t res_id,
                      const std::string& res_name, std::string* out);

}  // namespace apksx

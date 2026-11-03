// 二进制 AndroidManifest（AXML）最小解析：只产出"开始标签 + 属性"事件流。
// 完整 versionCode/permissions/components 的解析属于 M2，这里先把结构摊平，
// 让 M1 能拿到 <application android:icon>，M2 在同一事件流上继续加字段。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/common.h"

namespace apksx {

// Res_value dataType 里我们关心的几种
inline constexpr uint8_t kResTypeNull = 0x00;
inline constexpr uint8_t kResTypeReference = 0x01;
inline constexpr uint8_t kResTypeAttribute = 0x02;
inline constexpr uint8_t kResTypeString = 0x03;
inline constexpr uint8_t kResTypeIntDec = 0x10;
inline constexpr uint8_t kResTypeIntHex = 0x11;
inline constexpr uint8_t kResTypeIntBoolean = 0x12;

// android: 命名空间下的常用属性资源 ID（AOSP public.xml）
inline constexpr uint32_t kAttrLabel = 0x01010001u;
inline constexpr uint32_t kAttrIcon = 0x01010002u;
inline constexpr uint32_t kAttrName = 0x01010003u;
inline constexpr uint32_t kAttrMinSdkVersion = 0x0101020Cu;
inline constexpr uint32_t kAttrVersionCode = 0x0101021Bu;
inline constexpr uint32_t kAttrVersionName = 0x0101021Cu;
inline constexpr uint32_t kAttrTargetSdkVersion = 0x01010270u;
inline constexpr uint32_t kAttrRequired = 0x0101028Eu;  // android:required
inline constexpr uint32_t kAttrIsSplitRequired = 0x01010591u;

struct AxmlAttr {
  std::string name;
  uint32_t name_res_id = 0;
  uint8_t type = kResTypeNull;
  uint32_t data = 0;
  std::string string_value;  // rawValue 或 typed string，取到即存
  bool has_string = false;

  bool Is(uint32_t res_id, std::string_view fallback_name) const {
    return (name_res_id != 0 && name_res_id == res_id) ||
           (name_res_id == 0 && !fallback_name.empty() && name == fallback_name);
  }
};

struct AxmlElement {
  std::string tag;
  std::vector<AxmlAttr> attrs;
  int depth = 0;

  const AxmlAttr* Find(uint32_t res_id, std::string_view fallback_name) const {
    for (const AxmlAttr& a : attrs) {
      if (a.Is(res_id, fallback_name)) return &a;
    }
    return nullptr;
  }
};

// 解析整个文档的开始标签；失败返回 false（调用方按"无图标"处理）
bool ParseAxml(const std::vector<uint8_t>& data, std::vector<AxmlElement>* elements, Error* err);

// 从 <manifest> 里取属性的一站式小工具（找不到返回 false）
bool GetStringAttr(const AxmlElement& el, uint32_t res_id, std::string_view fallback_name,
                   std::string* out);
bool GetIntAttr(const AxmlElement& el, uint32_t res_id, std::string_view fallback_name,
                uint32_t* out);

}  // namespace apksx

// resources.arsc 的最小解析：只回答"这个资源 ID 在哪些配置里有值、值对应的 APK 内路径是什么"。
// M1 采用 TD-001 的整读策略（≤ 8 MB），M2 改成按 chunk 分段读。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/common.h"
#include "core/string_pool.h"

namespace apksx {

// 密度常量（ResTable_config::density）
inline constexpr uint16_t kDensityDefault = 0;
inline constexpr uint16_t kDensityLow = 120;
inline constexpr uint16_t kDensityMedium = 160;
inline constexpr uint16_t kDensityHigh = 240;
inline constexpr uint16_t kDensityXHigh = 320;
inline constexpr uint16_t kDensityXXHigh = 480;
inline constexpr uint16_t kDensityXXXHigh = 640;
inline constexpr uint16_t kDensityAny = 0xFFFEu;  // anydpi（多为 adaptive icon 的 XML）
inline constexpr uint16_t kDensityNone = 0xFFFFu;

// 一个 (配置, 值) 组合
struct ArscCandidate {
  uint8_t type_id = 0;
  uint32_t entry_index = 0;
  uint16_t density = kDensityDefault;
  uint16_t sdk_version = 0;
  uint8_t value_type = 0;
  uint32_t value_data = 0;      // TYPE_STRING 时是全局字符串池下标
  std::string value_string;     // 解析出的字符串（路径/名字）
  bool is_bag = false;          // 复杂条目（bag/map），M1 不用
};

struct ArscPackageInfo {
  uint8_t id = 0;
  std::string name;
};

class ArscTable {
 public:
  bool Parse(const std::vector<uint8_t>& data, Error* err);

  // 收集某个资源 ID 的全部候选（跨 density/sdk 配置）
  void CollectCandidates(uint32_t res_id, std::vector<ArscCandidate>* out) const;

  bool GetGlobalString(uint32_t index, std::string* out) const;
  const std::vector<ArscPackageInfo>& packages() const { return packages_; }
  // 供诊断：资源 ID 是否属于本表的某个 package
  bool HasPackage(uint8_t id) const;

 private:
  std::vector<uint8_t> data_;
  StringPool global_strings_;
  std::vector<ArscPackageInfo> packages_;
  bool parsed_ = false;
};

}  // namespace apksx

// 安装包元数据汇总（M2）：从 ByteSource 里抽取 apkinfo.exe 需要的字段。
// 全部只读，不落盘；每个字段都标注了来源（清单 / arsc / 中央目录）。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/byte_source.h"
#include "core/apk_signature.h"
#include "core/common.h"
#include "core/package.h"

namespace apksx {

struct ApkFeature {
  std::string name;          // 如 android.hardware.camera
  bool required = true;      // 清单里 android:required 缺省为 true
};

struct ComponentCounts {
  uint32_t activities = 0;  // 含 activity-alias
  uint32_t services = 0;
  uint32_t receivers = 0;
  uint32_t providers = 0;
};

struct ApkInfo {
  // file
  std::string path;
  uint64_t size = 0;
  uint64_t mtime_filetime = 0;
  std::string sha256;  // 仅在请求时填充

  // app（清单 + arsc）
  std::string package;
  std::string label;
  std::string version_name;
  uint32_t version_code = 0;
  uint32_t min_sdk = 0;
  uint32_t target_sdk = 0;
  std::vector<std::string> abis;       // 中央目录 lib/<abi>/...
  std::vector<std::string> densities;  // arsc 配置 + 条目路径
  std::vector<std::string> locales;    // arsc 配置的 language[-COUNTRY]

  // icon
  std::string icon_source;  // APK 内路径
  uint32_t icon_width = 0;
  uint32_t icon_height = 0;
  bool icon_adaptive = false;

  // components / permissions / features（清单）
  ComponentCounts components;
  std::vector<std::string> permissions;
  std::vector<ApkFeature> features;

  // 签名（APK Signing Block + META-INF 的 PKCS#7）
  SignatureInfo signature;

  // 容器（M4a）：单 APK 时 container = kApk、splits 为空
  ContainerKind container = ContainerKind::kApk;
  std::string base_entry;        // xapk/apks/apkm 里 base 的条目名
  std::vector<SplitApk> splits;  // 含 base
  bool icon_from_container = false;  // 图标来自容器根 icon.png（base 里没有）

  // diagnostics
  std::vector<std::string> warnings;
  double elapsed_ms = 0.0;
};

// 读路径上的文件（M2 主入口；worker 与测试共用）
bool CollectApkInfo(const std::wstring& file, bool want_sha256, ApkInfo* out, Error* err);

// 已打开的数据源版本（将来给管道/流用）
bool CollectApkInfoFromSource(std::shared_ptr<ByteSource> source, bool want_sha256, ApkInfo* out,
                              Error* err);

// 密度的数值 → 名字（ldpi/mdpi/.../anydpi/nodpi）
std::string DensityName(uint16_t density);

}  // namespace apksx

// 多 APK 容器（.apks / .xapk / .apkm）：外层 ZIP 里装 base.apk + 若干分包。
//
// 为什么单独一层：缩略图与属性页都要"先认出容器、再拿到 base.apk 的数据源"。
// base 是 Stored 时直接在外层文件上开窗口（SliceSource，零拷贝、1.25 GB 的 xapk 也不用读进内存）；
// 只有 deflate 的 base 才退化成整块解压（有上限）。
//
// 真实样本（tests/fixtures/real，不入库）：
//   Poweramp ...APKPure.xapk   3 条目：com.maxmpz.audioplayer.apk + config.armeabi_v7a.apk + manifest.json
//   PUBG ...APKPure.xapk       3 条目：com.tencent.ig.apk + Android/obb/... + manifest.json（1.25 GB）
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/byte_source.h"
#include "core/common.h"

namespace apksx {

class ZipReader;
class Deadline;

enum class ContainerKind {
  kApk = 0,  // 本身就是 APK（根上有 AndroidManifest.xml）
  kXapk,     // APKPure：manifest.json
  kApks,     // bundletool：toc.pb
  kApkm,     // APKMirror：info.json
  kSplits,   // 没有元数据的裸分包集合
  kHap,      // 鸿蒙 HAP：module.json
};

// "apk" / "xapk" / "apks" / "apkm" / "splits"（写进 JSON 与属性页）
const char* ContainerKindName(ContainerKind kind);

struct SplitApk {
  std::string name;  // 外层 ZIP 里的条目名
  uint64_t size = 0; // 未压缩大小
  std::string kind;  // "base" / "config" / "other"
  bool is_base = false;
};

struct PackageOpen {
  ContainerKind kind = ContainerKind::kApk;
  // base APK 的数据源。kApk 时为 nullptr（调用方直接用 outer）
  std::shared_ptr<ByteSource> apk;
  std::string base_entry;  // base 在外层里的名字；kApk 时为空
  uint64_t base_size = 0;
  std::string root_icon;   // 外层根上的 icon.png（base 里没图标时兜底）；可空
  std::vector<SplitApk> splits;  // 含 base 本身；kApk 时为空
};

// 判定容器并取出 base.apk。outer 必须是已打开中央目录的 ZIP（单 APK 也走这里）。
bool ResolvePackage(const ZipReader& outer, PackageOpen* out, Error* err);

// 便捷入口：按路径判定容器，out->apk 永远是可以直接 OpenSource 的数据源
// （单 APK 时就是文件本身，容器时是 base.apk 的窗口/内存块）。
bool OpenPackageFile(const std::wstring& path, PackageOpen* out, Error* err);

// 要画的那张图的原始字节：base 里的 launcher icon；找不到就退到容器根上的 icon.png。
// 不负责解码（调用方按需 WIC / PngSize）。
struct PackageIcon {
  std::vector<uint8_t> bytes;
  std::string source;  // "res/.../ic_launcher.png" 或 "com.x.apk!res/..." 或 "icon.png"
  std::string note;    // 诊断：为什么选它
  bool adaptive = false;
  bool from_container_root = false;
};

bool ReadPackageIcon(const ZipReader& outer, PackageIcon* out, Error* err, const Deadline* deadline);

}  // namespace apksx

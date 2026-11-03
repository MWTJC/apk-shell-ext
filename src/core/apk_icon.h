// APK 图标定位：ZIP 中央目录 → AndroidManifest.xml（AXML）→ resources.arsc → 条目路径。
// 这里是缩略图路径唯一的"业务逻辑"，M2 的 worker 会复用同一个 Manifest 解析结果。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/axml.h"
#include "core/common.h"

namespace apksx {

class ZipReader;

// 清单里缩略图/属性页都要的字段（M1 只用到 icon，其余为 M2 预留）
struct ManifestInfo {
  std::string package;
  std::string label;
  std::string version_name;
  uint32_t version_code = 0;
  uint32_t min_sdk = 0;
  uint32_t target_sdk = 0;
  bool has_icon = false;
  uint32_t icon_res_id = kInvalidResId;
  std::string icon_literal_path;  // android:icon 直接写成路径（少见）
};

struct IconLocation {
  std::string entry_path;      // APK 内路径，例如 res/mipmap-xxxhdpi-v4/ic_launcher.png
  uint16_t density = 0;        // 来自 arsc 配置；启发式选取时为 0
  bool adaptive_xml = false;   // 命中的是 anydpi 的 XML（M1 会尝试找光栅回退）
  bool heuristic = false;      // 走了"按文件名猜"的兜底路径
  std::string note;            // 诊断：为什么选它
};

// 软时限：所有耗时步骤之间检查，超预算即放弃（docs/design.md B1）
class Deadline {
 public:
  Deadline() : start_(Now()) {}
  explicit Deadline(int budget_ms) : start_(Now()), budget_ms_(budget_ms) {}
  double elapsed_ms() const;
  bool expired() const { return budget_ms_ > 0 && elapsed_ms() > static_cast<double>(budget_ms_); }
  int budget_ms() const { return budget_ms_; }

 private:
  static long long Now();
  long long start_ = 0;
  int budget_ms_ = 0;
};

// 打开文件 → 读清单 → 定位图标条目；不做解码。
// manifest 可为空；err 在返回 false 时给出面向用户的中文原因。
bool LocateApkIcon(const std::wstring& file, IconLocation* icon, ManifestInfo* manifest,
                   Error* err, const Deadline* deadline);

// 已经打开的 ZIP（stream 场景由调用方先 OpenSource）：读清单 + 定位图标
bool LocateApkIconInZip(const ZipReader& zip, IconLocation* icon, ManifestInfo* manifest,
                        Error* err, const Deadline* deadline);

// 把已经解析出来的 AXML 事件流汇总成清单字段（worker 与 provider 共用）
bool ParseManifestElements(const std::vector<AxmlElement>& elements, ManifestInfo* manifest,
                           Error* err);

// 分开的两步：属性页/worker 先要清单，再单独定位图标
bool ReadManifestFromZip(const ZipReader& zip, ManifestInfo* manifest, Error* err,
                         const Deadline* deadline);
bool LocateIconInZip(const ZipReader& zip, const ManifestInfo& info, IconLocation* icon,
                     Error* err, const Deadline* deadline);

// 只读清单（属性页 M2 用；M1 内部也走这条）
bool ReadApkManifest(const std::wstring& file, ManifestInfo* manifest, Error* err,
                     const Deadline* deadline);

}  // namespace apksx

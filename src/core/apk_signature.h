// APK 签名信息（M2）：方案识别（v1/v2/v3/v3.1）+ 签名证书摘要。
//
// 只做"显示用"的信息提取：不校验证书链、不重算内容摘要（那是 apksigner verify 的活，
// 代价与属性页"看一眼签名是谁"的需求不匹配）。证书摘要 = 证书 DER 的 SHA-256，
// 与 apksigner verify --print-certs 输出的 "certificate SHA-256 digest" 同源可比。
//
// v4 签名不在 APK 里（独立的 .idsig 文件），本模块不涉及。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/common.h"

namespace apksx {

class ZipReader;

struct SignatureCert {
  std::string subject;     // RFC 2253 风格（与 apksigner 同序：叶子在前）
  std::string sha256;      // 证书 DER 的 SHA-256，小写十六进制
  std::string not_before;  // ISO-8601 UTC，如 2024-01-01T00:00:00Z
  std::string not_after;
};

struct SignatureInfo {
  std::vector<std::string> schemes;  // 规范顺序 v1, v2, v3, v3.1
  std::vector<SignatureCert> certs;  // 按发现顺序，按 DER 摘要去重

  bool empty() const { return schemes.empty() && certs.empty(); }
};

// 读取签名信息。未签名 / 结构不认识都只记 warning，不算解析失败
// （现实里有未签名或重打包过的包，属性页仍应显示其它字段）。
void CollectApkSignature(const ZipReader& zip, SignatureInfo* out,
                         std::vector<std::string>* warnings);

// 测试用：APK Signing Block 里是否存在指定 id 的 pair（不解析证书）
bool HasSigningBlockId(const ZipReader& zip, uint32_t id);

}  // namespace apksx

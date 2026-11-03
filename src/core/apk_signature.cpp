#include "core/apk_signature.h"

#include <windows.h>
#include <wincrypt.h>

#include <cstdio>
#include <cstring>
#include <string_view>

#include "core/file_util.h"
#include "core/zip_reader.h"

namespace apksx {
namespace {

// APK Signing Block 内的 pair id（AOSP apksig ApkSigningBlockUtils）
constexpr uint32_t kSchemeV2 = 0x7109871au;
constexpr uint32_t kSchemeV3 = 0xf05368c0u;
constexpr uint32_t kSchemeV31 = 0x1b93ad61u;

// 块尾：[uint64 size][16 B magic]；size = 整块长度 - 8
constexpr char kBlockMagic[16] = {'A', 'P', 'K', ' ', 'S', 'i', 'g', ' ',
                                 'B', 'l', 'o', 'c', 'k', ' ', '4', '2'};
constexpr size_t kBlockFooterSize = 24;
constexpr uint64_t kMaxSigningBlock = 8ull * 1024 * 1024;
constexpr uint64_t kMaxV1BlockRead = 4ull * 1024 * 1024;

class Cursor {
 public:
  Cursor(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  size_t remaining() const { return size_ - pos_; }

  bool Skip(size_t n) {
    if (n > remaining()) return false;
    pos_ += n;
    return true;
  }

  // 长度前缀片段（u32 长度 + 内容）：返回指向原缓冲区内部的视图，不复制
  bool Lp(const uint8_t** out, size_t* len) {
    if (remaining() < 4) return false;
    const uint32_t n = le32(data_ + pos_);
    pos_ += 4;
    if (n > remaining()) return false;
    *out = data_ + pos_;
    *len = n;
    pos_ += n;
    return true;
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
};

bool StartsWithNoCase(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    const char a = static_cast<char>(s[i] >= 'A' && s[i] <= 'Z' ? s[i] - 'A' + 'a' : s[i]);
    const char b = static_cast<char>(prefix[i] >= 'A' && prefix[i] <= 'Z'
                                          ? prefix[i] - 'A' + 'a'
                                          : prefix[i]);
    if (a != b) return false;
  }
  return true;
}

bool EndsWithNoCase(std::string_view s, std::string_view suffix) {
  if (s.size() < suffix.size()) return false;
  const size_t off = s.size() - suffix.size();
  for (size_t i = 0; i < suffix.size(); ++i) {
    const char a =
        static_cast<char>(s[off + i] >= 'A' && s[off + i] <= 'Z' ? s[off + i] - 'A' + 'a'
                                                                 : s[off + i]);
    const char b = static_cast<char>(suffix[i] >= 'A' && suffix[i] <= 'Z'
                                          ? suffix[i] - 'A' + 'a'
                                          : suffix[i]);
    if (a != b) return false;
  }
  return true;
}

std::string IsoUtc(const FILETIME& ft) {
  SYSTEMTIME st{};
  if (!::FileTimeToSystemTime(&ft, &st)) return std::string();
  char buf[32];
  _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%04d-%02d-%02dT%02d:%02d:%02dZ",
              static_cast<int>(st.wYear), static_cast<int>(st.wMonth), static_cast<int>(st.wDay),
              static_cast<int>(st.wHour), static_cast<int>(st.wMinute),
              static_cast<int>(st.wSecond));
  return std::string(buf);
}

// 证书 DER → (摘要, 主题, 有效期)。摘要算不出来就当这张证书无效。
bool FillCert(PCCERT_CONTEXT ctx, SignatureCert* out) {
  std::wstring hex;
  std::string sha;
  if (!Sha256Hex(ctx->pbCertEncoded, ctx->cbCertEncoded, &hex) || !WideToUtf8(hex, &sha)) {
    return false;
  }
  out->sha256 = sha;

  // 主题按 RFC 2253 打印；REVERSE_FLAG 让 "CN=..." 在前，与 apksigner 输出一致
  const DWORD flags = CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG;
  const DWORD need = ::CertNameToStrW(X509_ASN_ENCODING, &ctx->pCertInfo->Subject, flags, nullptr, 0);
  if (need > 1) {
    std::wstring subject(static_cast<size_t>(need), L'\0');
    const DWORD written = ::CertNameToStrW(X509_ASN_ENCODING, &ctx->pCertInfo->Subject, flags,
                                           subject.data(), need);
    if (written > 0 && written <= need) {
      subject.resize(static_cast<size_t>(written) - 1);  // 去掉结尾 NUL
      WideToUtf8(subject, &out->subject);
    }
  }
  out->not_before = IsoUtc(ctx->pCertInfo->NotBefore);
  out->not_after = IsoUtc(ctx->pCertInfo->NotAfter);
  return true;
}

bool CertFromDer(const uint8_t* der, size_t len, SignatureCert* out) {
  PCCERT_CONTEXT ctx = ::CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, der,
                                                      static_cast<DWORD>(len));
  if (ctx == nullptr) return false;
  const bool ok = FillCert(ctx, out);
  ::CertFreeCertificateContext(ctx);
  return ok;
}

void AddCert(const SignatureCert& cert, SignatureInfo* out) {
  if (cert.sha256.empty()) return;
  for (const SignatureCert& existing : out->certs) {
    if (existing.sha256 == cert.sha256) return;  // 同一张证书在多个方案里重复出现
  }
  out->certs.push_back(cert);
}

// 读中央目录之前的 APK Signing Block。没有块（未签名 / 只有 v1）返回 false 且不报错。
bool ReadSigningBlock(const ZipReader& zip, std::vector<uint8_t>* block, size_t* pairs_end,
                      std::wstring* why) {
  const uint64_t cd = zip.cd_offset();
  if (cd < kBlockFooterSize) return false;

  uint8_t footer[kBlockFooterSize] = {};
  if (!zip.ReadRange(cd - kBlockFooterSize, footer, sizeof(footer))) return false;
  if (std::memcmp(footer + 8, kBlockMagic, sizeof(kBlockMagic)) != 0) return false;

  const uint64_t size = le64(footer);
  // size 里含块尾的第二个 size 字段与 magic，所以块起点在 cd - (size + 8)
  if (size < 24 || size > kMaxSigningBlock || size + 8 > cd) {
    if (why != nullptr) *why = L"APK 签名块长度异常";
    return false;
  }
  const uint64_t start = cd - (size + 8);
  block->resize(static_cast<size_t>(size + 8));
  if (!zip.ReadRange(start, block->data(), block->size())) {
    if (why != nullptr) *why = L"读取 APK 签名块失败";
    return false;
  }
  // 块布局：[size][pairs][size][magic]；size 不含第一个 8 字节字段，
  // 所以第一个 size 之后是 pairs，pairs 结束处（相对块起点 size-16）是第二个 size
  if (le64(block->data()) != size || le64(block->data() + size - 16) != size) {
    if (why != nullptr) *why = L"APK 签名块首尾长度不一致";
    return false;
  }
  *pairs_end = static_cast<size_t>(size - 16);
  return true;
}

bool FindPair(const std::vector<uint8_t>& block, size_t pairs_end, uint32_t wanted,
              const uint8_t** value, size_t* value_len) {
  size_t pos = 8;  // 跳过头部的 size
  while (pos + 12 <= pairs_end) {
    const uint64_t len = le64(block.data() + pos);  // 含 4 字节 id
    if (len < 4 || len > pairs_end - (pos + 8)) return false;
    const uint32_t id = le32(block.data() + pos + 8);
    if (id == wanted) {
      *value = block.data() + pos + 12;
      *value_len = static_cast<size_t>(len - 4);
      return true;
    }
    pos += static_cast<size_t>(8 + len);
  }
  return false;
}

// v2/v3/v3.1 的 value：
//   lp( signers 序列 )，序列里每个 signer 也是 lp( signer )
//   signer = lp(signed data) lp(signatures) lp(public key)
//   signed data = lp(digests) lp(certs) lp(attributes)（v3/v3.1 的 SDK 版本号在 certs 之后，
//   取证书用不到，这里不解析）
bool ParseSchemeSigners(const uint8_t* value, size_t value_len, SignatureInfo* out) {
  Cursor c(value, value_len);
  const uint8_t* signers = nullptr;
  size_t signers_len = 0;
  if (!c.Lp(&signers, &signers_len)) return false;

  bool any = false;
  Cursor sequence(signers, signers_len);
  while (sequence.remaining() >= 4) {
    const uint8_t* signer = nullptr;
    size_t signer_len = 0;
    if (!sequence.Lp(&signer, &signer_len)) break;

    Cursor s(signer, signer_len);
    const uint8_t* signed_data = nullptr;
    size_t signed_data_len = 0;
    if (!s.Lp(&signed_data, &signed_data_len)) continue;

    Cursor sd(signed_data, signed_data_len);
    const uint8_t* digests = nullptr;
    size_t digests_len = 0;
    const uint8_t* certs = nullptr;
    size_t certs_len = 0;
    if (!sd.Lp(&digests, &digests_len) || !sd.Lp(&certs, &certs_len)) continue;

    Cursor cc(certs, certs_len);
    while (cc.remaining() >= 4) {
      const uint8_t* der = nullptr;
      size_t der_len = 0;
      if (!cc.Lp(&der, &der_len)) break;
      SignatureCert cert;
      if (CertFromDer(der, der_len, &cert)) {
        AddCert(cert, out);
        any = true;
      }
    }
  }
  return any;
}

// v1：META-INF/*.(RSA|DSA|EC) 是 PKCS#7 SignedData，取每个块的第一张证书
bool ParseV1Block(const ZipReader& zip, const ZipEntry& entry, SignatureInfo* out) {
  std::vector<uint8_t> bytes;
  if (!zip.Read(entry, kMaxV1BlockRead, &bytes, nullptr)) return false;

  CERT_BLOB blob{};
  blob.cbData = static_cast<DWORD>(bytes.size());
  blob.pbData = bytes.data();

  HCERTSTORE store = nullptr;
  HCRYPTMSG message = nullptr;
  DWORD encoding = 0;
  DWORD content = 0;
  DWORD format = 0;
  if (!::CryptQueryObject(CERT_QUERY_OBJECT_BLOB, &blob, CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED,
                          CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &content, &format, &store,
                          &message, nullptr)) {
    return false;
  }

  bool any = false;
  PCCERT_CONTEXT ctx = ::CertEnumCertificatesInStore(store, nullptr);
  if (ctx != nullptr) {
    SignatureCert cert;
    if (FillCert(ctx, &cert)) {
      AddCert(cert, out);
      any = true;
    }
    ::CertFreeCertificateContext(ctx);
  }
  if (message != nullptr) ::CryptMsgClose(message);
  if (store != nullptr) ::CertCloseStore(store, 0);
  return any;
}

}  // namespace

void CollectApkSignature(const ZipReader& zip, SignatureInfo* out,
                         std::vector<std::string>* warnings) {
  if (out == nullptr) return;
  *out = SignatureInfo();
  auto warn = [warnings](const char* what) {
    if (warnings != nullptr) warnings->push_back(what);
  };

  // ---------------------------------------------------------------- v1（JAR 签名）
  int v1_blocks = 0;
  bool v1_ok = false;
  for (const ZipEntry& e : zip.entries()) {
    if (!StartsWithNoCase(e.name, "META-INF/")) continue;
    if (!EndsWithNoCase(e.name, ".RSA") && !EndsWithNoCase(e.name, ".DSA") &&
        !EndsWithNoCase(e.name, ".EC")) {
      continue;
    }
    ++v1_blocks;
    if (ParseV1Block(zip, e, out)) v1_ok = true;
  }
  if (v1_blocks > 0 && !v1_ok) warn("v1 签名块存在但无法解析（PKCS#7 结构异常）");

  // ---------------------------------------------------------------- v2 / v3 / v3.1
  bool v2_ok = false;
  bool v3_ok = false;
  bool v31_ok = false;
  std::vector<uint8_t> block;
  size_t pairs_end = 0;
  std::wstring why;
  if (ReadSigningBlock(zip, &block, &pairs_end, &why)) {
    const struct {
      uint32_t id;
      const char* name;
      bool* flag;
      const char* bad;
    } kSchemes[] = {
        {kSchemeV2, "v2", &v2_ok, "APK 签名块 v2 结构异常"},
        {kSchemeV3, "v3", &v3_ok, "APK 签名块 v3 结构异常"},
        {kSchemeV31, "v3.1", &v31_ok, "APK 签名块 v3.1 结构异常"},
    };
    for (const auto& scheme : kSchemes) {
      const uint8_t* value = nullptr;
      size_t value_len = 0;
      if (!FindPair(block, pairs_end, scheme.id, &value, &value_len)) continue;
      *(scheme.flag) = ParseSchemeSigners(value, value_len, out);
      if (!*(scheme.flag)) warn(scheme.bad);
    }
  } else if (!why.empty()) {
    // 块尾 magic 命中但结构不对：值得让用户看到（可能被工具改坏了）
    std::string message("APK 签名块无法解析：");
    std::string detail;
    if (WideToUtf8(why, &detail)) message += detail;
    warn(message.c_str());
  }

  // ---------------------------------------------------------------- 汇总
  if (v1_ok) out->schemes.push_back("v1");
  if (v2_ok) out->schemes.push_back("v2");
  if (v3_ok) out->schemes.push_back("v3");
  if (v31_ok) out->schemes.push_back("v3.1");
  if (out->schemes.empty()) warn("未发现签名信息（包可能未签名）");
}

bool HasSigningBlockId(const ZipReader& zip, uint32_t id) {
  std::vector<uint8_t> block;
  size_t pairs_end = 0;
  if (!ReadSigningBlock(zip, &block, &pairs_end, nullptr)) return false;
  const uint8_t* value = nullptr;
  size_t value_len = 0;
  return FindPair(block, pairs_end, id, &value, &value_len);
}

}  // namespace apksx

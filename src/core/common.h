// 公共常量与字节读取工具（docs/architecture.md §6、docs/formats.md §8）
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace apksx {

// ---------------------------------------------------------------- 安全限额
// 数值与 docs/formats.md §8 一一对应，改动须同步文档。
inline constexpr uint32_t kMaxZipEntries = 65535;
inline constexpr uint64_t kMaxEntryUncompressed = 64ull * 1024 * 1024;
inline constexpr uint64_t kMaxTotalUncompressed = 512ull * 1024 * 1024;
inline constexpr uint64_t kMaxManifestRead = 8ull * 1024 * 1024;
// TD-001：M1 允许整读 resources.arsc（≤ 8 MB），M2 改分段流式并删除此路径。
inline constexpr uint64_t kMaxArscRead = 8ull * 1024 * 1024;
inline constexpr uint64_t kMaxIconRead = 16ull * 1024 * 1024;
inline constexpr int kThumbBudgetMs = 300;

// 资源 ID 未找到时的哨兵
inline constexpr uint32_t kInvalidResId = 0xFFFFFFFFu;

// ---------------------------------------------------------------- 小工具
inline uint16_t le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
inline uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t le64(const uint8_t* p) {
  return static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32);
}

// 错误信息载体：静态库内部只用 std::wstring，跨 COM 边界时再转错误码。
struct Error {
  std::wstring message;
  void set(std::wstring msg) { message = std::move(msg); }
  bool empty() const { return message.empty(); }
};

// UTF-8（APK 内部条目名）与 UTF-16（Win32 路径）互转，失败时返回 false。
bool Utf8ToWide(std::string_view in, std::wstring* out);
bool WideToUtf8(std::wstring_view in, std::string* out);

}  // namespace apksx

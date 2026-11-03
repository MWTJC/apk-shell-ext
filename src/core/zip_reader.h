// ZIP 中央目录读取 + 单条目解压。
// 只做缩略图需要的两件事：按名字取条目、读出单个条目（raw deflate 或 store）。
// 不落盘、不解压整包（docs/formats.md §8 路径白名单在这里不做，因为从不写文件）。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/byte_source.h"
#include "core/common.h"

namespace apksx {

struct ZipEntry {
  std::string name;  // 原始字节（不转码；不信任字符集）
  uint16_t method = 0;
  uint16_t flags = 0;
  uint32_t crc32 = 0;
  uint64_t compressed_size = 0;
  uint64_t uncompressed_size = 0;
  uint64_t local_header_offset = 0;
};

class ZipReader {
 public:
  ZipReader() = default;
  ~ZipReader();
  ZipReader(const ZipReader&) = delete;
  ZipReader& operator=(const ZipReader&) = delete;

  // 打开文件并解析中央目录；失败时 err 带原因
  bool Open(const std::wstring& path, Error* err);
  // 接管一个数据源（file / stream）
  bool OpenSource(std::shared_ptr<ByteSource> source, Error* err);
  void Close();
  bool IsOpen() const { return source_ != nullptr; }

  uint64_t file_size() const { return file_size_; }
  const std::vector<ZipEntry>& entries() const { return entries_; }
  const ZipEntry* Find(std::string_view name) const;

  // 中央目录的 SHA-1（十六进制）：只有"裸文件名"的 stream 场景用它区分同名文件
  const std::string& cd_digest_hex() const { return cd_digest_; }

  // Stored（未压缩）条目的零拷贝视图；deflate 条目返回 nullptr（TD-001 的流式读靠它）
  std::shared_ptr<ByteSource> OpenStoredEntry(const ZipEntry& entry) const;

  // 解压单条目；limit 为解压后上限（同时用于防御声明值造假的炸弹）
  bool Read(const ZipEntry& entry, uint64_t limit, std::vector<uint8_t>* out, Error* err) const;

 private:
  bool ReadAt(uint64_t offset, void* buffer, size_t length) const;
  bool ParseCentralDirectory(uint64_t cd_offset, uint64_t cd_size, uint32_t expected_entries,
                             Error* err);

  std::shared_ptr<ByteSource> source_;
  uint64_t file_size_ = 0;
  std::vector<ZipEntry> entries_;
  std::string cd_digest_;
};

}  // namespace apksx

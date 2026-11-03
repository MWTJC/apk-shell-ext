// ResStringPool（resources.arsc 与二进制 AXML 共用的字符串池）。
// 只做"按索引取字符串"，风格池（styles）一律忽略。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/common.h"

namespace apksx {

class StringPool {
 public:
  // data 指向 RES_STRING_POOL_TYPE chunk 的起始位置；生命周期由调用方保证
  bool Parse(const uint8_t* data, size_t size, Error* err);

  size_t count() const { return offsets_.size(); }
  // UTF-8；索引越界或字符串损坏返回 false
  bool Get(size_t index, std::string* out) const;

 private:
  const uint8_t* chunk_ = nullptr;
  size_t chunk_size_ = 0;
  const uint8_t* strings_ = nullptr;
  size_t strings_size_ = 0;
  std::vector<uint32_t> offsets_;
  bool utf8_ = false;
};

// 读取 chunk 头（RES_CHUNK_HEADER）：type/headerSize/size
struct ChunkHeader {
  uint16_t type = 0;
  uint16_t header_size = 0;
  uint32_t size = 0;
};
inline ChunkHeader ReadChunkHeader(const uint8_t* p) {
  ChunkHeader h;
  h.type = le16(p);
  h.header_size = le16(p + 2);
  h.size = le32(p + 4);
  return h;
}

}  // namespace apksx

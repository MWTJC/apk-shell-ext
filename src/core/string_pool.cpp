#include "core/string_pool.h"

#include <cstring>

namespace apksx {
namespace {

constexpr uint16_t kResStringPoolType = 0x0001;
constexpr uint32_t kStringPoolUtf8Flag = 0x100;

}  // namespace

bool StringPool::Parse(const uint8_t* data, size_t size, Error* err) {
  chunk_ = data;
  chunk_size_ = size;
  offsets_.clear();

  if (data == nullptr || size < 28) {
    if (err) err->set(L"字符串池过小");
    return false;
  }
  const ChunkHeader h = ReadChunkHeader(data);
  if (h.type != kResStringPoolType) {
    if (err) err->set(L"不是字符串池 chunk");
    return false;
  }
  if (h.header_size < 28 || h.size > size || h.size < h.header_size) {
    if (err) err->set(L"字符串池头损坏");
    return false;
  }

  const uint32_t string_count = le32(data + 8);
  const uint32_t style_count = le32(data + 12);
  const uint32_t flags = le32(data + 16);
  const uint32_t strings_start = le32(data + 20);
  utf8_ = (flags & kStringPoolUtf8Flag) != 0;

  const size_t offsets_bytes = static_cast<size_t>(string_count) * 4;
  if (h.header_size + offsets_bytes > h.size) {
    if (err) err->set(L"字符串池偏移表越界");
    return false;
  }
  if (string_count == 0) {  // 空池在 aapt2 输出里合法
    strings_ = data;
    strings_size_ = 0;
    return true;
  }
  if (strings_start > h.size) {
    if (err) err->set(L"字符串池数据区越界");
    return false;
  }
  const uint32_t style_bytes = style_count * 4u;
  if (strings_start < h.header_size + offsets_bytes + style_bytes) {
    if (err) err->set(L"字符串池布局不一致");
    return false;
  }

  strings_ = data + strings_start;
  strings_size_ = h.size - strings_start;

  offsets_.resize(string_count);
  for (uint32_t i = 0; i < string_count; ++i) {
    const uint32_t off = le32(data + h.header_size + i * 4u);
    offsets_[i] = (off < strings_size_) ? off : 0xFFFFFFFFu;
  }
  return true;
}

bool StringPool::Get(size_t index, std::string* out) const {
  if (out == nullptr) return false;
  out->clear();
  if (index >= offsets_.size() || offsets_[index] == 0xFFFFFFFFu) return false;

  const uint8_t* p = strings_ + offsets_[index];
  const uint8_t* end = strings_ + strings_size_;

  if (utf8_) {
    if (p >= end) return false;
    // 两段变长长度：UTF-16 字符数（用于校验）与 UTF-8 字节数
    uint32_t u16len = *p++;
    if ((u16len & 0x80u) != 0) {
      if (p >= end) return false;
      u16len = ((u16len & 0x7Fu) << 8) | *p++;
    }
    if (p >= end) return false;
    uint32_t u8len = *p++;
    if ((u8len & 0x80u) != 0) {
      if (p >= end) return false;
      u8len = ((u8len & 0x7Fu) << 8) | *p++;
    }
    if (p + u8len > end) return false;
    out->assign(reinterpret_cast<const char*>(p), u8len);
    return true;
  }

  if (p + 2 > end) return false;
  uint32_t len = le16(p);
  p += 2;
  if ((len & 0x8000u) != 0) {
    if (p + 2 > end) return false;
    len = ((len & 0x7FFFu) << 16) | le16(p);
    p += 2;
  }
  if (p + len * 2u > end) return false;
  std::wstring wide(static_cast<size_t>(len), L'\0');
  for (uint32_t i = 0; i < len; ++i) wide[i] = static_cast<wchar_t>(le16(p + i * 2u));
  return WideToUtf8(wide, out);
}

}  // namespace apksx

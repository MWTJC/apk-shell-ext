#include "core/axml.h"

#include <cstdlib>
#include <cstring>
#include <string_view>

#include "core/string_pool.h"

namespace apksx {
namespace {

constexpr uint16_t kResXmlType = 0x0003;
constexpr uint16_t kResXmlResourceMapType = 0x0180;
constexpr uint16_t kResXmlStartElement = 0x0102;
constexpr uint16_t kResXmlEndElement = 0x0103;

constexpr size_t kMaxElements = 4096;
constexpr size_t kMaxAttrs = 256;

}  // namespace

bool ParseAxml(const std::vector<uint8_t>& data, std::vector<AxmlElement>* elements, Error* err) {
  if (elements == nullptr) return false;
  elements->clear();

  if (data.size() < 8 + 28) {
    if (err) err->set(L"AXML 文件太小");
    return false;
  }
  const ChunkHeader file = ReadChunkHeader(data.data());
  if (file.type != kResXmlType) {
    if (err) err->set(L"不是二进制 XML（AXML）");
    return false;
  }
  const size_t file_end = (file.size <= data.size()) ? file.size : data.size();

  StringPool pool;
  std::vector<uint32_t> resource_map;  // 索引 = 属性名在字符串池里的下标
  bool have_pool = false;

  size_t pos = file.header_size;
  int depth = 0;

  while (pos + 8 <= file_end) {
    const uint8_t* p = data.data() + pos;
    const ChunkHeader ch = ReadChunkHeader(p);
    if (ch.size < 8 || pos + ch.size > file_end) break;

    if (ch.type == 0x0001) {  // RES_STRING_POOL_TYPE
      if (!pool.Parse(p, ch.size, err)) return false;
      have_pool = true;
    } else if (ch.type == kResXmlResourceMapType) {
      const size_t count = (ch.size - ch.header_size) / 4u;
      resource_map.resize(count);
      for (size_t i = 0; i < count; ++i) {
        resource_map[i] = le32(p + ch.header_size + i * 4u);
      }
    } else if (ch.type == kResXmlStartElement) {
      if (!have_pool) {
        if (err) err->set(L"AXML 缺少字符串池");
        return false;
      }
      // ResXMLTree_node: header(8) + lineNumber(4) + comment(4)，随后是 attrExt
      if (ch.header_size < 16 || ch.size < static_cast<uint32_t>(ch.header_size) + 20u) {
        pos += ch.size;
        continue;
      }
      const uint8_t* ext = p + ch.header_size;
      const uint32_t name_idx = le32(ext + 4);
      const uint16_t attr_start = le16(ext + 8);
      const uint16_t attr_size = le16(ext + 10);
      const uint16_t attr_count = le16(ext + 12);

      AxmlElement el;
      el.depth = depth;
      if (!pool.Get(name_idx, &el.tag)) {
        pos += ch.size;
        ++depth;
        continue;
      }

      const size_t attrs_off = ch.header_size + attr_start;
      const size_t need = attrs_off + static_cast<size_t>(attr_count) * attr_size;
      if (attr_size < 20 || need > ch.size || attr_count > kMaxAttrs) {
        // 结构不可信：保留标签本身（例如 <application>），属性全丢
        el.attrs.clear();
      } else {
        el.attrs.reserve(attr_count);
        for (uint16_t i = 0; i < attr_count; ++i) {
          const uint8_t* a = p + attrs_off + static_cast<size_t>(i) * attr_size;
          const uint32_t attr_name_idx = le32(a + 4);
          const uint32_t raw_value_idx = le32(a + 8);

          AxmlAttr attr;
          if (!pool.Get(attr_name_idx, &attr.name)) {
            attr.name = "?";
          }
          if (attr_name_idx < resource_map.size()) {
            attr.name_res_id = resource_map[attr_name_idx];
          }
          // ResXMLTree_attribute: ns(0) name(4) rawValue(8) Res_value{size(12) res0(14) dataType(15) data(16)}
          attr.type = a[15];
          attr.data = le32(a + 16);
          if (raw_value_idx != 0xFFFFFFFFu) {
            attr.has_string = pool.Get(raw_value_idx, &attr.string_value);
          }
          if (!attr.has_string && attr.type == kResTypeString) {
            attr.has_string = pool.Get(attr.data, &attr.string_value);
          }
          el.attrs.push_back(std::move(attr));
        }
      }

      elements->push_back(std::move(el));
      if (elements->size() >= kMaxElements) break;
      ++depth;
    } else if (ch.type == kResXmlEndElement) {
      if (depth > 0) --depth;
    }

    pos += ch.size;
  }

  if (elements->empty()) {
    if (err) err->set(L"AXML 里没有元素");
    return false;
  }
  return true;
}

bool GetStringAttr(const AxmlElement& el, uint32_t res_id, std::string_view fallback_name,
                   std::string* out) {
  const AxmlAttr* a = el.Find(res_id, fallback_name);
  if (a == nullptr || !a->has_string) return false;
  if (out != nullptr) *out = a->string_value;
  return true;
}

bool GetIntAttr(const AxmlElement& el, uint32_t res_id, std::string_view fallback_name,
                uint32_t* out) {
  const AxmlAttr* a = el.Find(res_id, fallback_name);
  if (a == nullptr) return false;
  if (a->type == kResTypeIntDec || a->type == kResTypeIntHex || a->type == kResTypeIntBoolean) {
    if (out != nullptr) *out = a->data;
    return true;
  }
  if (a->has_string) {
    // 少数包把版本号写成字符串
    char* end = nullptr;
    const unsigned long v = std::strtoul(a->string_value.c_str(), &end, 10);
    if (end != nullptr && *end == '\0' && v <= 0xFFFFFFFFu) {
      if (out != nullptr) *out = static_cast<uint32_t>(v);
      return true;
    }
  }
  return false;
}

}  // namespace apksx

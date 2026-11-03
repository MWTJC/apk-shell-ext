// resources.arsc 的流式读取（关闭 TD-001）。
//
// 为什么不整读：arsc 随语言/密度数量线性膨胀，几十 MB 的表在真实游戏包里很常见。
// APK 里的 resources.arsc 几乎总是 Stored（未压缩，便于 mmap），所以可以直接在
// 文件上开窗口、按 chunk 头跳读：
//   - 找图标：读表头 + 全局池头 + 目标 type chunk 的头 + 单个条目（几百字节级）
//   - 汇总语言/密度：读所有 type chunk 的**头**（每个 ~80 B），不碰内容
//   - 取字符串：读池的偏移表一项 + 该字符串本身
// deflate 的 arsc 无法随机访问，调用方回退到有界整读（见 apk_icon / apk_info）。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/arsc.h"
#include "core/byte_source.h"
#include "core/common.h"

namespace apksx {

class ArscStreamTable {
 public:
  // arsc 可以是一个 SliceSource（指向 ZIP 里 Stored 条目的窗口）
  bool Open(std::shared_ptr<ByteSource> arsc, Error* err);

  void CollectCandidates(uint32_t res_id, std::vector<ArscCandidate>* out) const;
  void CollectConfigs(std::vector<ArscConfig>* out) const;
  bool GetGlobalString(uint32_t index, std::string* out) const;

  bool ok() const { return size_ != 0; }
  uint64_t size() const { return size_; }

 private:
  struct Package {
    uint8_t id = 0;
    uint8_t type_id_offset = 0;
    uint16_t header_size = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
  };

  bool ReadAt(uint64_t offset, void* buffer, size_t length) const;
  bool ReadStringAt(uint64_t pool_offset, uint64_t pool_size, uint32_t index,
                    std::string* out) const;
  void ScanTypeChunk(uint64_t chunk_offset, uint16_t header_size, uint32_t chunk_size,
                     uint8_t type_id_offset, uint8_t want_type, uint32_t want_entry,
                     std::vector<ArscCandidate>* out) const;

  std::shared_ptr<ByteSource> src_;
  uint64_t size_ = 0;
  uint64_t global_pool_offset_ = 0;
  uint64_t global_pool_size_ = 0;
  std::vector<Package> packages_;
};

}  // namespace apksx

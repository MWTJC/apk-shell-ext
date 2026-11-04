// 随机读取源：同一套解析代码要能吃"文件路径"和"COM IStream"两种输入。
// 为什么需要 stream：explorer 把缩略图 handler 放在 dllhost.exe 隔离宿主里跑，
// 只通过 IInitializeWithStream 喂数据，且 STATSTG.pwcsName 只有裸文件名（无路径）。
//
// M2 新增 SliceSource：APK 里 resources.arsc 绝大多数是 Stored（未压缩），
// 可以直接在文件上开一个"窗口"按需读，不必整表读进内存（TD-001）。
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/common.h"
#include "core/com_ptr.h"

namespace apksx {

class ByteSource {
 public:
  virtual ~ByteSource() = default;
  virtual uint64_t size() const = 0;
  // 越界或 IO 失败返回 false
  virtual bool ReadAt(uint64_t offset, void* buffer, size_t length) = 0;
};

std::shared_ptr<ByteSource> OpenFileSource(const std::wstring& path, Error* err);
std::shared_ptr<ByteSource> OpenStreamSource(IStream* stream, Error* err);

// 在 parent 上开一个 [offset, offset+length) 的只读窗口（不复制数据）
std::shared_ptr<ByteSource> SliceSource(const std::shared_ptr<ByteSource>& parent,
                                        uint64_t offset, uint64_t length);

// 内存数据源：容器里的 base.apk 是 deflate 时只能先解压出来（不能开窗口）
std::shared_ptr<ByteSource> MemorySource(std::vector<uint8_t> bytes);

}  // namespace apksx

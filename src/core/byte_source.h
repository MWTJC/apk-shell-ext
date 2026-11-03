// 随机读取源：同一套解析代码要能吃"文件路径"和"COM IStream"两种输入。
// 为什么需要 stream：explorer 把缩略图 handler 放在 dllhost.exe 隔离宿主里跑，
// 只通过 IInitializeWithStream 喂数据，且 STATSTG.pwcsName 只有裸文件名（无路径）。
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

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

std::unique_ptr<ByteSource> OpenFileSource(const std::wstring& path, Error* err);
std::unique_ptr<ByteSource> OpenStreamSource(IStream* stream, Error* err);

}  // namespace apksx

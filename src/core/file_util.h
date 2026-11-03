// 文件与路径小工具：只在 Win32 API 上实现，不引入任何第三方依赖。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace apksx {

struct FileStamp {
  uint64_t size = 0;
  uint64_t mtimeFileTime = 0;  // FILETIME 作为 64 位整数（100ns 单位）
  bool valid = false;
};

// 用 GetFileAttributesExW 取元数据：不产生 CreateFile（V1 判据要求缓存命中路径零打开）。
bool GetFileStamp(const std::wstring& path, FileStamp* out);

// SHA-1 / SHA-256 十六进制（小写）
bool Sha1Hex(const void* data, size_t size, std::wstring* hex);
bool Sha256Hex(const void* data, size_t size, std::wstring* hex);

// %LOCALAPPDATA%\apk-shell-ext（按需创建，末尾不带反斜杠）
bool GetDataDir(std::wstring* out);

// 递归创建目录（已存在视为成功）
bool EnsureDirectory(const std::wstring& path);

// 有界读文件（用于测试与 worker）：超过 limit 直接失败
bool ReadWholeFile(const std::wstring& path, uint64_t limit, std::vector<uint8_t>* out);

// 原子写：先写 <path>.tmp 再 MoveFileEx(REPLACE_EXISTING)
bool AtomicWriteFile(const std::wstring& path, const void* data, size_t size);

// 路径拼接与取扩展名（小写、含点）
std::wstring JoinPath(const std::wstring& dir, const std::wstring& leaf);
std::wstring LowerExtension(const std::wstring& path);

}  // namespace apksx

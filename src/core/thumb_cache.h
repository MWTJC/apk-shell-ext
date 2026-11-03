// 缩略图自建缓存：%LOCALAPPDATA%\apk-shell-ext\thumbs\<key>.png
// key = sha1(path | size | mtime | 解析器版本)（docs/architecture.md §3）
// 命中路径不打开源文件；负缓存 24 h。
#pragma once

#include <cstdint>
#include <string>

#include "core/common.h"
#include "core/file_util.h"

namespace apksx {

class DecodedImage;

struct CacheLookup {
  bool hit = false;          // 有可用缩略图
  bool negative = false;     // 负缓存命中（24 h 内不重试）
  std::wstring png_path;     // hit 时的 PNG 路径
};

class ThumbCache {
 public:
  // 缓存键材料：路径模式 = path|size|mtime|parser
  static bool MakePathMaterial(const std::wstring& file, const FileStamp& stamp,
                               std::wstring* material);
  // 缓存键材料：stream 模式 = name|size|mtime|cdHash|parser（名字可能只是裸文件名）
  static std::wstring MakeStreamMaterial(const std::wstring& name, uint64_t size,
                                        uint64_t mtime, const std::string& cd_digest);

  // file 必须存在；失败（文件不存在等）返回 false
  static bool Lookup(const std::wstring& file, CacheLookup* out);
  // 把解码好的图像写成 PNG；stamp 用于算键
  static bool Store(const std::wstring& file, const FileStamp& stamp, const DecodedImage& image);
  static void StoreNegative(const std::wstring& file, const FileStamp& stamp);

  // 显式键材料的版本（provider 的 stream 分支使用）
  static bool LookupByMaterial(const std::wstring& material, CacheLookup* out);
  static bool StoreByMaterial(const std::wstring& material, const DecodedImage& image);
  static void StoreNegativeByMaterial(const std::wstring& material);

  static std::wstring RootDir();
  // 清理整个缓存目录（tools/thumbtest --clean-cache、M5 的清理工具共用）
  static bool ClearAll(uint64_t* removed_bytes);

 private:
  static void MaybePrune(uint64_t limit_bytes);
};

}  // namespace apksx

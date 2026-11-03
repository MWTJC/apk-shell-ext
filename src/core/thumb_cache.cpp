#include "core/thumb_cache.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/wic_image.h"

namespace apksx {
namespace {

constexpr uint64_t kNegativeTtlSeconds = 24ull * 3600ull;
constexpr uint64_t kCacheLimitBytes = 200ull * 1024 * 1024;
constexpr const wchar_t* kSubDir = L"thumbs";

uint64_t FileAgeSeconds(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
  FILETIME now{};
  ::GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER a{}, b{};
  a.LowPart = now.dwLowDateTime;
  a.HighPart = now.dwHighDateTime;
  b.LowPart = fad.ftLastWriteTime.dwLowDateTime;
  b.HighPart = fad.ftLastWriteTime.dwHighDateTime;
  if (a.QuadPart <= b.QuadPart) return 0;
  return (a.QuadPart - b.QuadPart) / 10000000ull;
}

bool FileBytes(const std::wstring& path, uint64_t* out) {
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
  *out = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
  return true;
}

}  // namespace

std::wstring ThumbCache::RootDir() {
  std::wstring base;
  if (!GetDataDir(&base)) return std::wstring();
  const std::wstring dir = JoinPath(base, kSubDir);
  if (!EnsureDirectory(dir)) return std::wstring();
  return dir;
}

bool ThumbCache::MakePathMaterial(const std::wstring& file, const FileStamp& stamp,
                                  std::wstring* material) {
  std::string utf8;
  if (!WideToUtf8(file, &utf8)) return false;
  std::string text = utf8;
  text += '|';
  text += std::to_string(stamp.size);
  text += '|';
  text += std::to_string(stamp.mtimeFileTime);
  text += '|';
  text += APKSX_PARSER_VERSION;
  return Utf8ToWide(text, material);
}

std::wstring ThumbCache::MakeStreamMaterial(const std::wstring& name, uint64_t size,
                                            uint64_t mtime, const std::string& cd_digest) {
  std::string utf8_name;
  WideToUtf8(name, &utf8_name);
  std::string text = "stream:";
  text += utf8_name;
  text += '|';
  text += std::to_string(size);
  text += '|';
  text += std::to_string(mtime);
  text += '|';
  text += cd_digest;
  text += '|';
  text += APKSX_PARSER_VERSION;
  std::wstring wide;
  if (!Utf8ToWide(text, &wide)) return std::wstring();
  return wide;
}

bool ThumbCache::Lookup(const std::wstring& file, CacheLookup* out) {
  if (out == nullptr) return false;
  FileStamp stamp;
  if (!GetFileStamp(file, &stamp)) return false;
  std::wstring material;
  if (!MakePathMaterial(file, stamp, &material)) return false;
  return LookupByMaterial(material, out);
}

bool ThumbCache::LookupByMaterial(const std::wstring& material, CacheLookup* out) {
  if (out == nullptr) return false;
  *out = CacheLookup();

  std::string utf8;
  if (!WideToUtf8(material, &utf8) || utf8.empty()) return false;
  std::wstring key;
  if (!Sha1Hex(utf8.data(), utf8.size(), &key)) return false;

  const std::wstring dir = RootDir();
  if (dir.empty()) return true;  // 没有缓存目录：命中不了，但不是错误

  const std::wstring png = JoinPath(dir, key + L".png");
  uint64_t bytes = 0;
  if (FileBytes(png, &bytes) && bytes > 0) {
    out->hit = true;
    out->png_path = png;
    return true;
  }

  const std::wstring fail = JoinPath(dir, key + L".fail");
  if (FileBytes(fail, &bytes)) {
    if (FileAgeSeconds(fail) < kNegativeTtlSeconds) {
      out->negative = true;
    } else {
      ::DeleteFileW(fail.c_str());  // 过期负缓存：当作未命中
    }
  }
  return true;
}

bool ThumbCache::Store(const std::wstring& file, const FileStamp& stamp,
                       const DecodedImage& image) {
  std::wstring material;
  if (!MakePathMaterial(file, stamp, &material)) return false;
  return StoreByMaterial(material, image);
}

bool ThumbCache::StoreByMaterial(const std::wstring& material, const DecodedImage& image) {
  if (!image.valid()) return false;
  std::string utf8;
  if (!WideToUtf8(material, &utf8) || utf8.empty()) return false;
  std::wstring key;
  if (!Sha1Hex(utf8.data(), utf8.size(), &key)) return false;

  const std::wstring dir = RootDir();
  if (dir.empty()) return false;

  std::vector<uint8_t> png;
  Error err;
  if (!image.EncodePng(&png, &err) || png.empty()) {
    DebugLogf(L"cache encode failed: %ls", err.message.c_str());
    return false;
  }
  const std::wstring path = JoinPath(dir, key + L".png");
  if (!AtomicWriteFile(path, png.data(), png.size())) {
    DebugLogf(L"cache write failed: %ls", path.c_str());
    return false;
  }
  // 成功后删掉同键的负缓存
  ::DeleteFileW(JoinPath(dir, key + L".fail").c_str());
  MaybePrune(kCacheLimitBytes);
  return true;
}

void ThumbCache::StoreNegative(const std::wstring& file, const FileStamp& stamp) {
  std::wstring material;
  if (!MakePathMaterial(file, stamp, &material)) return;
  StoreNegativeByMaterial(material);
}

void ThumbCache::StoreNegativeByMaterial(const std::wstring& material) {
  std::string utf8;
  if (!WideToUtf8(material, &utf8) || utf8.empty()) return;
  std::wstring key;
  if (!Sha1Hex(utf8.data(), utf8.size(), &key)) return;
  const std::wstring dir = RootDir();
  if (dir.empty()) return;
  // 内容只需存在；mtime 就是"何时失败"，用于 24 h 过期
  const char marker[] = "negative";
  AtomicWriteFile(JoinPath(dir, key + L".fail"), marker, sizeof(marker) - 1);
}

void ThumbCache::MaybePrune(uint64_t limit_bytes) {
  const std::wstring dir = RootDir();
  if (dir.empty()) return;

  WIN32_FIND_DATAW fd{};
  HANDLE h = ::FindFirstFileW(JoinPath(dir, L"*.png").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;

  struct Item {
    std::wstring path;
    uint64_t bytes;
    uint64_t age_seconds;
  };
  std::vector<Item> items;
  uint64_t total = 0;
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    const std::wstring full = JoinPath(dir, fd.cFileName);
    Item it;
    it.path = full;
    it.bytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    it.age_seconds = FileAgeSeconds(full);
    total += it.bytes;
    items.push_back(std::move(it));
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);

  if (total <= limit_bytes) return;

  // 最老的先删（近似 LRU：mtime 即最后写入时间）
  std::sort(items.begin(), items.end(),
            [](const Item& a, const Item& b) { return a.age_seconds > b.age_seconds; });
  for (const Item& it : items) {
    if (total <= limit_bytes) break;
    if (::DeleteFileW(it.path.c_str())) total -= it.bytes;
  }
  DebugLogf(L"cache pruned to %llu bytes", static_cast<unsigned long long>(total));
}

bool ThumbCache::ClearAll(uint64_t* removed_bytes) {
  const std::wstring dir = RootDir();
  if (dir.empty()) return false;

  WIN32_FIND_DATAW fd{};
  HANDLE h = ::FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return true;

  uint64_t removed = 0;
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    const std::wstring full = JoinPath(dir, fd.cFileName);
    const uint64_t bytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    if (::DeleteFileW(full.c_str())) removed += bytes;
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);

  if (removed_bytes != nullptr) *removed_bytes = removed;
  return true;
}

}  // namespace apksx

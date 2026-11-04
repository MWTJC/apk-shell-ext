#include "thumbs/thumb_provider.h"

#include <shobjidl.h>
#include <thumbcache.h>
#include <windows.h>

#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "core/apk_icon.h"
#include "core/byte_source.h"
#include "core/log.h"
#include "core/package.h"
#include "core/thumb_cache.h"
#include "core/wic_image.h"
#include "core/zip_reader.h"
#include "thumbs/apkthumb.h"

namespace {

using apksx::CacheLookup;
using apksx::DecodedImage;
using apksx::Deadline;
using apksx::Error;
using apksx::FileStamp;
using apksx::IconLocation;
using apksx::ThumbCache;
using apksx::ZipEntry;
using apksx::ZipReader;

// 缓存里保存的源图最长边（docs/formats.md §7："只解码到 ≤ 512×512"）
constexpr uint32_t kMaxSourceDim = 512;

// 两种输入模式：
//   1) file   —— 有真实路径（IInitializeWithItem / IInitializeWithFile / 测试）
//   2) stream —— explorer 的隔离缩略图宿主只给 IStream，STATSTG.pwcsName 只有裸文件名
class ThumbProvider final : public IThumbnailProvider,
                            public IInitializeWithItem,
                            public IInitializeWithFile,
                            public IInitializeWithStream {
 public:
  ThumbProvider() { IncrementServerLock(); }

  // ---------------------------------------------------------------- IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown) {
      *ppv = static_cast<IThumbnailProvider*>(this);
    } else if (riid == IID_IThumbnailProvider) {
      *ppv = static_cast<IThumbnailProvider*>(this);
    } else if (riid == IID_IInitializeWithItem) {
      *ppv = static_cast<IInitializeWithItem*>(this);
    } else if (riid == IID_IInitializeWithFile) {
      *ppv = static_cast<IInitializeWithFile*>(this);
    } else if (riid == IID_IInitializeWithStream) {
      *ppv = static_cast<IInitializeWithStream*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(::InterlockedIncrement(&refs_));
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const long remaining = ::InterlockedDecrement(&refs_);
    if (remaining == 0) {
      DecrementServerLock();
      delete this;
    }
    return static_cast<ULONG>(remaining);
  }

  // --------------------------------------------- IInitializeWithItem / File
  HRESULT STDMETHODCALLTYPE Initialize(IShellItem* item, DWORD) override {
    if (item == nullptr) return E_INVALIDARG;
    PWSTR raw = nullptr;
    const HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    if (FAILED(hr) || raw == nullptr) return E_FAIL;
    {
      std::lock_guard<std::mutex> guard(lock_);
      path_.assign(raw);
      stream_.Reset();
    }
    ::CoTaskMemFree(raw);
    apksx::DebugLogf(L"Initialize(IShellItem) -> %ls", path_.c_str());
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR file_path, DWORD) override {
    if (file_path == nullptr) return E_INVALIDARG;
    std::lock_guard<std::mutex> guard(lock_);
    path_.assign(file_path);
    stream_.Reset();
    apksx::DebugLogf(L"Initialize(IInitializeWithFile) -> %ls", path_.c_str());
    return S_OK;
  }

  // 隔离宿主只给流：保留流本身 + 元数据（大小/mtime/名字），解析全部走流。
  HRESULT STDMETHODCALLTYPE Initialize(IStream* stream, DWORD) override {
    if (stream == nullptr) return E_INVALIDARG;
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_DEFAULT))) return E_FAIL;

    std::wstring name;
    if (stat.pwcsName != nullptr) {
      name.assign(stat.pwcsName);
      ::CoTaskMemFree(stat.pwcsName);
    }
    const uint64_t size = static_cast<uint64_t>(stat.cbSize.QuadPart);
    const uint64_t mtime =
        (static_cast<uint64_t>(stat.mtime.dwHighDateTime) << 32) | stat.mtime.dwLowDateTime;

    apksx::ComPtr<IStream> clone;
    if (FAILED(stream->Clone(clone.Put())) || !clone) {
      clone.Attach(stream);
      stream->AddRef();
    }

    {
      std::lock_guard<std::mutex> guard(lock_);
      stream_ = clone;
      stream_name_ = name;
      stream_size_ = size;
      stream_mtime_ = mtime;
      path_.clear();
    }
    apksx::DebugLogf(L"Initialize(IInitializeWithStream) name=%ls size=%llu", name.c_str(),
                     static_cast<unsigned long long>(size));
    return S_OK;
  }

  // ------------------------------------------------------- IThumbnailProvider
  HRESULT STDMETHODCALLTYPE GetThumbnail(UINT cx, HBITMAP* phbmp,
                                         WTS_ALPHATYPE* pdw_alpha) override {
    if (phbmp == nullptr) return E_POINTER;
    *phbmp = nullptr;
    if (pdw_alpha != nullptr) *pdw_alpha = WTSAT_ARGB;

    // DLL 在 explorer / dllhost 里跑：任何异常都不许跨边界
    try {
      return GetThumbnailImpl(cx, phbmp);
    } catch (...) {
      apksx::DebugLog(L"GetThumbnail: 未捕获异常");
      if (*phbmp != nullptr) {
        ::DeleteObject(*phbmp);
        *phbmp = nullptr;
      }
      return E_FAIL;
    }
  }

 private:
  ~ThumbProvider() = default;

  HRESULT BuildFromCache(const std::wstring& png_path, UINT cx, HBITMAP* phbmp) {
    DecodedImage cached;
    Error err;
    if (!apksx::DecodePngFile(png_path, kMaxSourceDim, &cached, &err)) return E_FAIL;
    return cached.CreateHBitmap(cx, phbmp, nullptr, nullptr, &err) ? S_OK : E_FAIL;
  }

  // 把图标条目读进内存 → 解码 → 落缓存 → 出 HBITMAP。
  // 容器（.xapk/.apks/.apkm）由 ReadPackageIcon 负责取出 base.apk；
  // base 里没有图标时它自己退到容器根上的 icon.png。
  HRESULT BuildFromZip(ZipReader& zip, const std::wstring& material, UINT cx, HBITMAP* phbmp,
                       const Deadline& deadline) {
    Error err;

    apksx::PackageIcon icon;
    if (!apksx::ReadPackageIcon(zip, &icon, &err, &deadline)) {
      apksx::DebugLogf(L"locate failed (%ls) key=%ls", err.message.c_str(), material.c_str());
      ThumbCache::StoreNegativeByMaterial(material);
      return E_FAIL;
    }

    std::vector<uint8_t> bytes = std::move(icon.bytes);

    DecodedImage image;
    const double decode_start = deadline.elapsed_ms();
    if (!image.DecodeFromMemory(bytes, kMaxSourceDim, &err)) {
      apksx::DebugLogf(L"decode failed (%ls): %hs key=%ls", err.message.c_str(),
                       icon.source.c_str(), material.c_str());
      ThumbCache::StoreNegativeByMaterial(material);
      return E_FAIL;
    }
    const double decode_ms = deadline.elapsed_ms() - decode_start;

    // 先落缓存（失败不影响出图）
    ThumbCache::StoreByMaterial(material, image);

    if (!image.CreateHBitmap(cx, phbmp, nullptr, nullptr, &err)) {
      apksx::DebugLogf(L"CreateHBitmap failed: %ls", err.message.c_str());
      return E_FAIL;
    }

    apksx::DebugLogf(L"built %.1fms (decode %.1fms) %ux%u %hs (%hs)", deadline.elapsed_ms(),
                     decode_ms, cx, cx, icon.source.c_str(), icon.note.c_str());
    return S_OK;
  }

  HRESULT GetThumbnailImpl(UINT cx, HBITMAP* phbmp) {
    std::wstring path;
    apksx::ComPtr<IStream> stream;
    std::wstring stream_name;
    uint64_t stream_size = 0;
    uint64_t stream_mtime = 0;
    {
      std::lock_guard<std::mutex> guard(lock_);
      path = path_;
      stream = stream_;
      stream_name = stream_name_;
      stream_size = stream_size_;
      stream_mtime = stream_mtime_;
    }
    if (cx == 0) cx = 256;

    const Deadline deadline(apksx::kThumbBudgetMs);

    // ---------------------------------------------------------------- 文件模式
    if (!path.empty()) {
      CacheLookup cache;
      if (ThumbCache::Lookup(path, &cache)) {
        if (cache.negative) {
          apksx::DebugLogf(L"negative cache hit: %ls", path.c_str());
          return E_FAIL;
        }
        if (cache.hit) {
          const HRESULT hr = BuildFromCache(cache.png_path, cx, phbmp);
          apksx::DebugLogf(L"cache hit %.1fms hr=0x%08lX %ls", deadline.elapsed_ms(),
                           static_cast<unsigned long>(hr), path.c_str());
          if (SUCCEEDED(hr)) return hr;
        }
      }

      FileStamp stamp;
      if (!apksx::GetFileStamp(path, &stamp)) return E_FAIL;
      std::wstring material;
      if (!ThumbCache::MakePathMaterial(path, stamp, &material)) return E_FAIL;

      ZipReader zip;
      if (!zip.Open(path, nullptr)) {
        ThumbCache::StoreNegativeByMaterial(material);
        return E_FAIL;
      }
      return BuildFromZip(zip, material, cx, phbmp, deadline);
    }

    // ---------------------------------------------------------------- 流模式
    if (!stream) return E_UNEXPECTED;

    auto source = apksx::OpenStreamSource(stream.Get(), nullptr);
    if (!source) return E_FAIL;

    ZipReader zip;
    if (!zip.OpenSource(source, nullptr)) return E_FAIL;

    // 流只有裸文件名：缓存键必须带上中央目录摘要，否则同名不同包会串图
    const uint64_t size = (stream_size != 0) ? stream_size : zip.file_size();
    const std::wstring material =
        ThumbCache::MakeStreamMaterial(stream_name.empty() ? L"<stream>" : stream_name, size,
                                       stream_mtime, zip.cd_digest_hex());
    if (material.empty()) return E_FAIL;

    CacheLookup cache;
    if (ThumbCache::LookupByMaterial(material, &cache)) {
      if (cache.negative) {
        apksx::DebugLogf(L"negative cache hit (stream) %ls", stream_name.c_str());
        return E_FAIL;
      }
      if (cache.hit) {
        const HRESULT hr = BuildFromCache(cache.png_path, cx, phbmp);
        apksx::DebugLogf(L"cache hit (stream) %.1fms hr=0x%08lX",
                         deadline.elapsed_ms(), static_cast<unsigned long>(hr));
        if (SUCCEEDED(hr)) return hr;
      }
    }

    return BuildFromZip(zip, material, cx, phbmp, deadline);
  }

  volatile long refs_ = 1;
  std::mutex lock_;
  std::wstring path_;
  apksx::ComPtr<IStream> stream_;
  std::wstring stream_name_;
  uint64_t stream_size_ = 0;
  uint64_t stream_mtime_ = 0;
};

}  // namespace

HRESULT CreateThumbProvider(REFIID riid, void** ppv) {
  if (ppv == nullptr) return E_POINTER;
  *ppv = nullptr;
  auto* provider = new (std::nothrow) ThumbProvider();
  if (provider == nullptr) return E_OUTOFMEMORY;
  const HRESULT hr = provider->QueryInterface(riid, ppv);
  provider->Release();
  return hr;
}

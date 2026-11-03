#include "core/byte_source.h"

#include <algorithm>

namespace apksx {
namespace {

class FileSource final : public ByteSource {
 public:
  explicit FileSource(HANDLE file, uint64_t size) : file_(file), size_(size) {}
  ~FileSource() override {
    if (file_ != INVALID_HANDLE_VALUE) ::CloseHandle(file_);
  }

  uint64_t size() const override { return size_; }

  bool ReadAt(uint64_t offset, void* buffer, size_t length) override {
    if (file_ == INVALID_HANDLE_VALUE) return false;
    if (offset > size_ || length > size_ - offset) return false;
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(file_, pos, nullptr, FILE_BEGIN)) return false;

    auto* out = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < length) {
      const DWORD want =
          static_cast<DWORD>((length - done) > 0x10000000u ? 0x10000000u : (length - done));
      DWORD got = 0;
      if (!::ReadFile(file_, out + done, want, &got, nullptr) || got == 0) return false;
      done += got;
    }
    return true;
  }

 private:
  HANDLE file_;
  uint64_t size_;
};

class StreamSource final : public ByteSource {
 public:
  StreamSource(ComPtr<IStream> stream, uint64_t size) : stream_(std::move(stream)), size_(size) {}
  ~StreamSource() override = default;

  uint64_t size() const override { return size_; }

  bool ReadAt(uint64_t offset, void* buffer, size_t length) override {
    if (!stream_) return false;
    if (offset > size_ || length > size_ - offset) return false;
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (FAILED(stream_->Seek(pos, STREAM_SEEK_SET, nullptr))) return false;

    auto* out = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < length) {
      const ULONG want = static_cast<ULONG>(
          std::min<size_t>(length - done, 0x10000000u));
      ULONG got = 0;
      if (FAILED(stream_->Read(out + done, want, &got)) || got == 0) return false;
      done += got;
    }
    return true;
  }

 private:
  ComPtr<IStream> stream_;
  uint64_t size_;
};

}  // namespace

std::unique_ptr<ByteSource> OpenFileSource(const std::wstring& path, Error* err) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (err) err->set(L"无法打开文件");
    return nullptr;
  }
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0) {
    ::CloseHandle(file);
    if (err) err->set(L"无法获取文件大小");
    return nullptr;
  }
  return std::make_unique<FileSource>(file, static_cast<uint64_t>(size.QuadPart));
}

std::unique_ptr<ByteSource> OpenStreamSource(IStream* stream, Error* err) {
  if (stream == nullptr) {
    if (err) err->set(L"stream 为空");
    return nullptr;
  }
  ComPtr<IStream> owned;
  if (FAILED(stream->Clone(owned.Put())) || !owned) {
    // Clone 失败就用原 stream（handler 生命周期内它由 shell 持有）
    owned.Attach(stream);
    stream->AddRef();
  }
  STATSTG stat{};
  if (FAILED(owned->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart < 0) {
    if (err) err->set(L"无法获取 stream 大小");
    return nullptr;
  }
  LARGE_INTEGER zero{};
  owned->Seek(zero, STREAM_SEEK_SET, nullptr);
  return std::make_unique<StreamSource>(std::move(owned),
                                        static_cast<uint64_t>(stat.cbSize.QuadPart));
}

}  // namespace apksx

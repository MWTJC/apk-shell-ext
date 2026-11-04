#include "core/byte_source.h"

#include <algorithm>
#include <cstring>
#include <vector>

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
      const ULONG want = static_cast<ULONG>(std::min<size_t>(length - done, 0x10000000u));
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

// 在一个已有数据源上开窗口：只做偏移换算，不拥有数据
class RegionSource final : public ByteSource {
 public:
  RegionSource(std::shared_ptr<ByteSource> parent, uint64_t offset, uint64_t length)
      : parent_(std::move(parent)), offset_(offset), length_(length) {}

  uint64_t size() const override { return length_; }

  bool ReadAt(uint64_t offset, void* buffer, size_t length) override {
    if (offset > length_ || length > length_ - offset) return false;
    return parent_->ReadAt(offset_ + offset, buffer, length);
  }

 private:
  std::shared_ptr<ByteSource> parent_;
  uint64_t offset_;
  uint64_t length_;
};

// 整块内存。只给"必须解压才能随机读"的场景用（容器里 deflate 的 base.apk）。
class BlobSource final : public ByteSource {
 public:
  explicit BlobSource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}

  uint64_t size() const override { return static_cast<uint64_t>(bytes_.size()); }

  bool ReadAt(uint64_t offset, void* buffer, size_t length) override {
    if (offset > bytes_.size() || length > bytes_.size() - offset) return false;
    if (length > 0) std::memcpy(buffer, bytes_.data() + offset, length);
    return true;
  }

 private:
  std::vector<uint8_t> bytes_;
};

}  // namespace

std::shared_ptr<ByteSource> OpenFileSource(const std::wstring& path, Error* err) {
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
  return std::make_shared<FileSource>(file, static_cast<uint64_t>(size.QuadPart));
}

std::shared_ptr<ByteSource> OpenStreamSource(IStream* stream, Error* err) {
  if (stream == nullptr) {
    if (err) err->set(L"stream 为空");
    return nullptr;
  }
  ComPtr<IStream> owned;
  if (FAILED(stream->Clone(owned.Put())) || !owned) {
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
  return std::make_shared<StreamSource>(std::move(owned),
                                        static_cast<uint64_t>(stat.cbSize.QuadPart));
}

std::shared_ptr<ByteSource> SliceSource(const std::shared_ptr<ByteSource>& parent,
                                        uint64_t offset, uint64_t length) {
  if (!parent) return nullptr;
  if (offset > parent->size()) return nullptr;
  const uint64_t available = parent->size() - offset;
  return std::make_shared<RegionSource>(parent, offset, std::min(length, available));
}

std::shared_ptr<ByteSource> MemorySource(std::vector<uint8_t> bytes) {
  return std::make_shared<BlobSource>(std::move(bytes));
}

}  // namespace apksx

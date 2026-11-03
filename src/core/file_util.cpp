#include "core/file_util.h"

#include <windows.h>
#include <bcrypt.h>

#include <cwctype>
#include <string>

#include "core/common.h"

#pragma comment(lib, "bcrypt.lib")

namespace apksx {
namespace {

bool HashHex(LPCWSTR algorithm, size_t digest_len, const void* data, size_t size,
             std::wstring* hex) {
  if (hex == nullptr) return false;

  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, algorithm, nullptr, 0) < 0) return false;

  uint8_t digest[64] = {};
  // BCryptHash 的输出长度必须等于算法自身的摘要长度（给大了会返回 STATUS_INVALID_PARAMETER）
  const NTSTATUS st = BCryptHash(alg, nullptr, 0, static_cast<PUCHAR>(const_cast<void*>(data)),
                                 static_cast<ULONG>(size), digest,
                                 static_cast<ULONG>(digest_len));
  BCryptCloseAlgorithmProvider(alg, 0);
  if (st < 0) return false;

  // 摘要长度按算法固定：SHA-1 = 20，SHA-256 = 32
  static const wchar_t kDigits[] = L"0123456789abcdef";
  hex->clear();
  hex->reserve(digest_len * 2);
  for (size_t i = 0; i < digest_len; ++i) {
    hex->push_back(kDigits[digest[i] >> 4]);
    hex->push_back(kDigits[digest[i] & 0xF]);
  }
  return true;
}

}  // namespace

bool Utf8ToWide(std::string_view in, std::wstring* out) {
  if (out == nullptr) return false;
  out->clear();
  if (in.empty()) return true;
  const int need = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in.data(),
                                         static_cast<int>(in.size()), nullptr, 0);
  if (need <= 0) return false;
  out->resize(static_cast<size_t>(need));
  const int got = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in.data(),
                                        static_cast<int>(in.size()), out->data(), need);
  return got == need;
}

bool WideToUtf8(std::wstring_view in, std::string* out) {
  if (out == nullptr) return false;
  out->clear();
  if (in.empty()) return true;
  const int need = ::WideCharToMultiByte(CP_UTF8, 0, in.data(), static_cast<int>(in.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (need <= 0) return false;
  out->resize(static_cast<size_t>(need));
  const int got = ::WideCharToMultiByte(CP_UTF8, 0, in.data(), static_cast<int>(in.size()),
                                        out->data(), need, nullptr, nullptr);
  return got == need;
}

bool GetFileStamp(const std::wstring& path, FileStamp* out) {
  if (out == nullptr) return false;
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
  out->size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
  out->mtimeFileTime =
      (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
      fad.ftLastWriteTime.dwLowDateTime;
  out->valid = true;
  return true;
}

bool Sha1Hex(const void* data, size_t size, std::wstring* hex) {
  return HashHex(BCRYPT_SHA1_ALGORITHM, 20, data, size, hex);
}

bool Sha256Hex(const void* data, size_t size, std::wstring* hex) {
  return HashHex(BCRYPT_SHA256_ALGORITHM, 32, data, size, hex);
}

bool Sha256FileHex(const std::wstring& path, std::wstring* hex, Error* err) {
  if (hex == nullptr) return false;

  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (err) err->set(L"无法打开文件计算哈希");
    return false;
  }

  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
  if (ok) {
    DWORD object_size = 0;
    DWORD written = 0;
    ok = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                           sizeof(object_size), &written, 0) >= 0 &&
         object_size > 0;
    if (ok) {
      std::vector<uint8_t> object(object_size);
      ok = BCryptCreateHash(alg, &hash, object.data(), object_size, nullptr, 0, 0) >= 0;
      if (ok) {
        std::vector<uint8_t> buffer(1u << 20);
        for (;;) {
          DWORD read = 0;
          if (!::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            break;
          }
          if (read == 0) break;
          if (BCryptHashData(hash, buffer.data(), read, 0) < 0) {
            ok = false;
            break;
          }
        }
        uint8_t digest[32] = {};
        if (ok) {
          ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
          if (ok) {
            static const wchar_t kDigits[] = L"0123456789abcdef";
            hex->clear();
            hex->reserve(64);
            for (uint8_t b : digest) {
              hex->push_back(kDigits[b >> 4]);
              hex->push_back(kDigits[b & 0xF]);
            }
          }
        }
      }
    }
  }

  if (hash != nullptr) BCryptDestroyHash(hash);
  if (alg != nullptr) BCryptCloseAlgorithmProvider(alg, 0);
  ::CloseHandle(file);

  if (!ok && err) err->set(L"计算 SHA-256 失败");
  return ok;
}

bool GetDataDir(std::wstring* out) {
  if (out == nullptr) return false;
  wchar_t buf[MAX_PATH * 2] = {};
  const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, ARRAYSIZE(buf));
  if (n == 0 || n >= ARRAYSIZE(buf)) return false;
  std::wstring dir = buf;
  dir += L"\\apk-shell-ext";
  if (!EnsureDirectory(dir)) return false;
  *out = std::move(dir);
  return true;
}

bool EnsureDirectory(const std::wstring& path) {
  if (path.empty()) return false;
  if (::CreateDirectoryW(path.c_str(), nullptr)) return true;
  return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

bool ReadWholeFile(const std::wstring& path, uint64_t limit, std::vector<uint8_t>* out) {
  if (out == nullptr) return false;
  out->clear();

  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;

  LARGE_INTEGER size{};
  bool ok = ::GetFileSizeEx(h, &size) != FALSE;
  if (ok) {
    if (size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > limit) {
      ok = false;
    } else {
      out->resize(static_cast<size_t>(size.QuadPart));
      size_t done = 0;
      while (ok && done < out->size()) {
        const DWORD want = static_cast<DWORD>(
            (out->size() - done) > 0x10000000ull ? 0x10000000ull : (out->size() - done));
        DWORD got = 0;
        ok = ::ReadFile(h, out->data() + done, want, &got, nullptr) != FALSE && got > 0;
        done += got;
      }
    }
  }
  ::CloseHandle(h);
  if (!ok) out->clear();
  return ok;
}

bool AtomicWriteFile(const std::wstring& path, const void* data, size_t size) {
  const std::wstring tmp = path + L".tmp";
  HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;

  bool ok = true;
  size_t done = 0;
  while (ok && done < size) {
    const DWORD want =
        static_cast<DWORD>((size - done) > 0x10000000ull ? 0x10000000ull : (size - done));
    DWORD wrote = 0;
    ok = ::WriteFile(h, static_cast<const uint8_t*>(data) + done, want, &wrote, nullptr) != FALSE &&
         wrote > 0;
    done += wrote;
  }
  ::CloseHandle(h);

  if (ok) ok = ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
  if (!ok) ::DeleteFileW(tmp.c_str());
  return ok;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& leaf) {
  std::wstring out = dir;
  if (!out.empty() && out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
  out += leaf;
  return out;
}

std::wstring LowerExtension(const std::wstring& path) {
  const size_t dot = path.find_last_of(L'.');
  const size_t sep = path.find_last_of(L"\\/");
  if (dot == std::wstring::npos) return std::wstring();
  if (sep != std::wstring::npos && dot < sep) return std::wstring();
  std::wstring ext = path.substr(dot);
  for (wchar_t& c : ext) c = static_cast<wchar_t>(::towlower(c));
  return ext;
}

}  // namespace apksx

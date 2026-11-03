// WIC 解码（PNG/WebP/JPEG）与缩放 → HBITMAP。
// 只用系统 WIC，不引第三方解码器（ADR-0004 / V8：规避 libwebp CVE 类问题）。
#pragma once

#include <windows.h>
#include <wincodec.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/common.h"
#include "core/com_ptr.h"

namespace apksx {

// 解码后的图像（自然尺寸，最长边不超过 max_dim）
class DecodedImage {
 public:
  bool DecodeFromMemory(const std::vector<uint8_t>& bytes, uint32_t max_dim, Error* err);
  // 缩放并生成 32bpp premultiplied BGRA 的 DIB section
  bool CreateHBitmap(uint32_t target_cx, HBITMAP* out, uint32_t* out_cx, uint32_t* out_cy,
                     Error* err) const;
  // 编码成 PNG 字节（缓存用；保持自然尺寸）
  bool EncodePng(std::vector<uint8_t>* out, Error* err) const;

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  bool valid() const { return source_.Get() != nullptr; }

 private:
  ComPtr<IWICBitmapSource> source_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

// 从缓存文件（我们自己写的 PNG）解码
bool DecodePngFile(const std::wstring& path, uint32_t max_dim, DecodedImage* out, Error* err);

}  // namespace apksx

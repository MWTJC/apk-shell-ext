#include "core/wic_image.h"

#include <shlwapi.h>
#include <wincodec.h>

#include <algorithm>

#include "core/file_util.h"
#include "core/log.h"

namespace apksx {
namespace {

constexpr uint64_t kMaxDecodedFileBytes = 8ull * 1024 * 1024;

// WIC 工厂按进程缓存：explorer 里只创建一次（V4 内存判据要求增量 ≤ 3 MB）。
IWICImagingFactory* WicFactory() {
  static IWICImagingFactory* factory = []() -> IWICImagingFactory* {
    IWICImagingFactory* f = nullptr;
    const HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&f));
    if (FAILED(hr)) {
      DebugLogf(L"CoCreateInstance(WIC) failed hr=0x%08lX", static_cast<unsigned long>(hr));
      return nullptr;
    }
    return f;  // 故意不释放：进程生命周期内复用
  }();
  return factory;
}

bool Fail(Error* err, const wchar_t* msg, HRESULT hr) {
  if (err) {
    wchar_t buf[256];
    _snwprintf_s(buf, _TRUNCATE, L"%s（hr=0x%08lX）", msg, static_cast<unsigned long>(hr));
    err->set(buf);
  }
  return false;
}

// 把任意 frame 转成 32bpp PBGRA，并按 max_dim 限制最长边
bool PrepareSource(IWICImagingFactory* factory, IWICBitmapSource* raw, uint32_t max_dim,
                   ComPtr<IWICBitmapSource>* out, uint32_t* out_w, uint32_t* out_h, Error* err) {
  UINT w = 0;
  UINT h = 0;
  HRESULT hr = raw->GetSize(&w, &h);
  if (FAILED(hr) || w == 0 || h == 0) return Fail(err, L"图像尺寸无效", hr);

  ComPtr<IWICBitmapSource> source;
  if (w > max_dim || h > max_dim) {
    const double scale =
        static_cast<double>(max_dim) / static_cast<double>(std::max(w, h));
    const UINT nw = std::max<UINT>(1, static_cast<UINT>(static_cast<double>(w) * scale + 0.5));
    const UINT nh = std::max<UINT>(1, static_cast<UINT>(static_cast<double>(h) * scale + 0.5));
    ComPtr<IWICBitmapScaler> scaler;
    hr = factory->CreateBitmapScaler(scaler.Put());
    if (FAILED(hr)) return Fail(err, L"创建缩放器失败", hr);
    hr = scaler->Initialize(raw, nw, nh, WICBitmapInterpolationModeHighQualityCubic);
    if (FAILED(hr)) return Fail(err, L"缩放失败", hr);
    if (!scaler.As(&source)) return Fail(err, L"查询位图源失败", E_NOINTERFACE);
    w = nw;
    h = nh;
  } else {
    hr = raw->QueryInterface(IID_PPV_ARGS(source.Put()));
    if (FAILED(hr)) return Fail(err, L"查询位图源失败", hr);
  }

  ComPtr<IWICFormatConverter> converter;
  hr = factory->CreateFormatConverter(converter.Put());
  if (FAILED(hr)) return Fail(err, L"创建格式转换器失败", hr);
  hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                             nullptr, 0.0, WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) return Fail(err, L"像素格式转换失败", hr);

  ComPtr<IWICBitmapSource> result;
  if (!converter.As(&result)) return Fail(err, L"查询转换结果失败", E_NOINTERFACE);
  *out = std::move(result);
  if (out_w != nullptr) *out_w = w;
  if (out_h != nullptr) *out_h = h;
  return true;
}

}  // namespace

bool DecodedImage::DecodeFromMemory(const std::vector<uint8_t>& bytes, uint32_t max_dim,
                                    Error* err) {
  source_.Reset();
  width_ = 0;
  height_ = 0;

  IWICImagingFactory* factory = WicFactory();
  if (factory == nullptr) {
    if (err) err->set(L"WIC 不可用");
    return false;
  }
  if (bytes.empty()) {
    if (err) err->set(L"图像数据为空");
    return false;
  }

  ComPtr<IStream> stream;
  stream.Attach(::SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
  if (!stream) {
    if (err) err->set(L"创建内存流失败");
    return false;
  }

  ComPtr<IWICBitmapDecoder> decoder;
  HRESULT hr = factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                WICDecodeMetadataCacheOnDemand, decoder.Put());
  if (FAILED(hr)) return Fail(err, L"WIC 无法解码该图像（格式不支持或数据损坏）", hr);

  ComPtr<IWICBitmapFrameDecode> frame;
  hr = decoder->GetFrame(0, frame.Put());
  if (FAILED(hr)) return Fail(err, L"WIC 取帧失败", hr);

  return PrepareSource(factory, frame.Get(), max_dim, &source_, &width_, &height_, err);
}

bool DecodedImage::CreateHBitmap(uint32_t target_cx, HBITMAP* out, uint32_t* out_cx,
                                 uint32_t* out_cy, Error* err) const {
  if (out == nullptr) return false;
  *out = nullptr;
  if (source_.Get() == nullptr || width_ == 0 || height_ == 0) {
    if (err) err->set(L"没有可用的图像");
    return false;
  }
  if (target_cx == 0) target_cx = 256;
  if (target_cx > 1024) target_cx = 1024;

  IWICImagingFactory* factory = WicFactory();
  if (factory == nullptr) {
    if (err) err->set(L"WIC 不可用");
    return false;
  }

  // 等比缩放到 target_cx 的方框内（图标一般是正方形，这里对非正方形也安全）
  const double scale = static_cast<double>(target_cx) / static_cast<double>(std::max(width_, height_));
  const UINT cx = std::max<UINT>(1, static_cast<UINT>(static_cast<double>(width_) * scale + 0.5));
  const UINT cy = std::max<UINT>(1, static_cast<UINT>(static_cast<double>(height_) * scale + 0.5));

  ComPtr<IWICBitmapScaler> scaler;
  HRESULT hr = factory->CreateBitmapScaler(scaler.Put());
  if (FAILED(hr)) return Fail(err, L"创建缩放器失败", hr);
  hr = scaler->Initialize(source_.Get(), cx, cy, WICBitmapInterpolationModeHighQualityCubic);
  if (FAILED(hr)) return Fail(err, L"缩放图像失败", hr);

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = static_cast<LONG>(cx);
  bmi.bmiHeader.biHeight = -static_cast<LONG>(cy);  // 自上而下
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP hbmp = ::CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (hbmp == nullptr || bits == nullptr) {
    if (hbmp != nullptr) ::DeleteObject(hbmp);
    if (err) err->set(L"CreateDIBSection 失败");
    return false;
  }

  const UINT stride = cx * 4u;
  hr = scaler->CopyPixels(nullptr, stride, stride * cy, static_cast<BYTE*>(bits));
  if (FAILED(hr)) {
    ::DeleteObject(hbmp);
    return Fail(err, L"拷贝像素失败", hr);
  }

  *out = hbmp;
  if (out_cx != nullptr) *out_cx = cx;
  if (out_cy != nullptr) *out_cy = cy;
  return true;
}

bool DecodedImage::EncodePng(std::vector<uint8_t>* out, Error* err) const {
  if (out == nullptr) return false;
  out->clear();
  if (source_.Get() == nullptr) {
    if (err) err->set(L"没有可编码的图像");
    return false;
  }

  IWICImagingFactory* factory = WicFactory();
  if (factory == nullptr) {
    if (err) err->set(L"WIC 不可用");
    return false;
  }

  ComPtr<IStream> stream;
  HGLOBAL global = nullptr;
  HRESULT hr = ::CreateStreamOnHGlobal(nullptr, TRUE,
                                       reinterpret_cast<IStream**>(stream.Put()));
  if (FAILED(hr)) return Fail(err, L"创建流失败", hr);

  ComPtr<IWICBitmapEncoder> encoder;
  hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.Put());
  if (FAILED(hr)) return Fail(err, L"创建 PNG 编码器失败", hr);
  hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  if (FAILED(hr)) return Fail(err, L"初始化编码器失败", hr);

  ComPtr<IWICBitmapFrameEncode> frame;
  hr = encoder->CreateNewFrame(frame.Put(), nullptr);
  if (FAILED(hr)) return Fail(err, L"创建编码帧失败", hr);
  hr = frame->Initialize(nullptr);
  if (FAILED(hr)) return Fail(err, L"初始化编码帧失败", hr);
  hr = frame->SetSize(width_, height_);
  if (FAILED(hr)) return Fail(err, L"设置尺寸失败", hr);

  WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
  hr = frame->SetPixelFormat(&format);
  if (FAILED(hr)) return Fail(err, L"设置像素格式失败", hr);
  hr = frame->WriteSource(source_.Get(), nullptr);
  if (FAILED(hr)) return Fail(err, L"写入图像失败", hr);
  hr = frame->Commit();
  if (FAILED(hr)) return Fail(err, L"提交帧失败", hr);
  hr = encoder->Commit();
  if (FAILED(hr)) return Fail(err, L"提交编码失败", hr);

  hr = ::GetHGlobalFromStream(stream.Get(), &global);
  if (FAILED(hr) || global == nullptr) return Fail(err, L"取编码结果失败", hr);
  const SIZE_T size = ::GlobalSize(global);
  const void* data = ::GlobalLock(global);
  if (data == nullptr || size == 0) {
    if (data != nullptr) ::GlobalUnlock(global);
    if (err) err->set(L"编码结果为空");
    return false;
  }
  out->assign(static_cast<const uint8_t*>(data),
              static_cast<const uint8_t*>(data) + static_cast<size_t>(size));
  ::GlobalUnlock(global);
  return true;
}

bool DecodePngFile(const std::wstring& path, uint32_t max_dim, DecodedImage* out, Error* err) {
  if (out == nullptr) return false;
  std::vector<uint8_t> bytes;
  if (!ReadWholeFile(path, kMaxDecodedFileBytes, &bytes)) {
    if (err) err->set(L"读取缩略图缓存失败");
    return false;
  }
  return out->DecodeFromMemory(bytes, max_dim, err);
}

}  // namespace apksx

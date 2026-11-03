#include <windows.h>
#include <new>
#include <unknwn.h>

#include "thumbs/apkthumb.h"
#include "thumbs/thumb_provider.h"

namespace {

class ClassFactory final : public IClassFactory {
 public:
  ClassFactory() { IncrementServerLock(); }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
      *ppv = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
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

  // IClassFactory
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (outer != nullptr) return CLASS_E_NOAGGREGATION;
    return CreateThumbProvider(riid, ppv);
  }

  HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
    if (lock != FALSE) {
      IncrementServerLock();
    } else {
      DecrementServerLock();
    }
    return S_OK;
  }

 private:
  ~ClassFactory() = default;
  volatile long refs_ = 1;
};

}  // namespace

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
  if (ppv == nullptr) return E_POINTER;
  *ppv = nullptr;

  try {
    if (::IsEqualCLSID(rclsid, CLSID_ApkThumbProvider)) {
      auto* factory = new (std::nothrow) ClassFactory();
      if (factory == nullptr) return E_OUTOFMEMORY;
      const HRESULT hr = factory->QueryInterface(riid, ppv);
      factory->Release();
      return hr;
    }
  } catch (...) {
    return E_UNEXPECTED;
  }
  return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() {
  return (ServerLockCount() == 0) ? S_OK : S_FALSE;
}

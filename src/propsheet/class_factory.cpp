#include <windows.h>

#include <new>
#include <unknwn.h>

#include "core/log.h"
#include "propsheet/apkprop.h"

namespace {

class ClassFactory final : public IClassFactory {
 public:
  ClassFactory() { IncrementServerLock(); }

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

  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (outer != nullptr) return CLASS_E_NOAGGREGATION;
    const HRESULT hr = CreatePropertyPage(riid, ppv);
    apksx::DebugLogf(L"apkprop: CreateInstance riid=%08lX -> 0x%08lX ptr=%p",
                     static_cast<unsigned long>(riid.Data1), static_cast<unsigned long>(hr),
                     ppv == nullptr ? nullptr : *ppv);
    return hr;
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
  apksx::DebugLogf(L"apkprop: DllGetClassObject rclsid=%08lX-%04X-%04X riid=%08lX",
                   static_cast<unsigned long>(rclsid.Data1),
                   static_cast<unsigned>(rclsid.Data2), static_cast<unsigned>(rclsid.Data3),
                   static_cast<unsigned long>(riid.Data1));

  try {
    if (::IsEqualCLSID(rclsid, CLSID_ApkPropertyPage)) {
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

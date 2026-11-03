// 最小 ComPtr：项目不引入 wil（ADR-0004 允许但未 vendor），手写一份够用即可。
#pragma once

#include <unknwn.h>

namespace apksx {

template <typename T>
class ComPtr {
 public:
  ComPtr() = default;
  ComPtr(const ComPtr& other) : p_(other.p_) {
    if (p_ != nullptr) p_->AddRef();
  }
  ComPtr(ComPtr&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
  ~ComPtr() { Reset(); }

  ComPtr& operator=(const ComPtr& other) {
    if (this != &other) {
      ComPtr tmp(other);
      Swap(tmp);
    }
    return *this;
  }
  ComPtr& operator=(ComPtr&& other) noexcept {
    if (this != &other) {
      Reset();
      p_ = other.p_;
      other.p_ = nullptr;
    }
    return *this;
  }

  void Swap(ComPtr& other) noexcept {
    T* tmp = p_;
    p_ = other.p_;
    other.p_ = tmp;
  }

  void Reset() {
    if (p_ != nullptr) {
      p_->Release();
      p_ = nullptr;
    }
  }

  T* Get() const { return p_; }
  T** Put() {
    Reset();
    return &p_;
  }
  // 接管一个已持有引用的裸指针
  void Attach(T* p) {
    Reset();
    p_ = p;
  }
  T* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }

  // QueryInterface 到另一接口（保留原引用）
  template <typename U>
  bool As(ComPtr<U>* out) const {
    if (out == nullptr) return false;
    out->Reset();
    if (p_ == nullptr) return false;
    return p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out->Put())) == S_OK;
  }

 private:
  T* p_ = nullptr;
};

}  // namespace apksx

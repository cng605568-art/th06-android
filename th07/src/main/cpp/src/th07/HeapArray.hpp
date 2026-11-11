#pragma once

// A fixed-size array kept on the heap (the bullet, item and enemy pools are too large for the
// objects that own them to live on the stack) that copies deeply, so its owner keeps value
// semantics: a practice save state is a plain copy of the game's systems.

#include <array>
#include <cstddef>
#include <memory>

namespace touhou::th07 {

template <typename T, std::size_t N>
class HeapArray {
  public:
    HeapArray() : p_(std::make_unique<std::array<T, N>>()) {}
    HeapArray(const HeapArray& o) : p_(std::make_unique<std::array<T, N>>(*o.p_)) {}
    HeapArray& operator=(const HeapArray& o) {
        if (this != &o) *p_ = *o.p_;  // into the same storage: pointers to it stay valid
        return *this;
    }
    HeapArray(HeapArray&&) noexcept = default;
    HeapArray& operator=(HeapArray&&) noexcept = default;

    T& operator[](std::size_t i) { return (*p_)[i]; }
    const T& operator[](std::size_t i) const { return (*p_)[i]; }
    T* data() { return p_->data(); }
    const T* data() const { return p_->data(); }
    static constexpr std::size_t size() { return N; }
    auto begin() { return p_->begin(); }
    auto end() { return p_->end(); }
    auto begin() const { return p_->cbegin(); }
    auto end() const { return p_->cend(); }
    std::array<T, N>& array() { return *p_; }
    const std::array<T, N>& array() const { return *p_; }

  private:
    std::unique_ptr<std::array<T, N>> p_;
};

// One heap object with the same deep-copy semantics.
template <typename T>
class HeapBox {
  public:
    HeapBox() : p_(std::make_unique<T>()) {}
    HeapBox(const HeapBox& o) : p_(std::make_unique<T>(*o.p_)) {}
    HeapBox& operator=(const HeapBox& o) {
        if (this != &o) *p_ = *o.p_;
        return *this;
    }
    HeapBox(HeapBox&&) noexcept = default;
    HeapBox& operator=(HeapBox&&) noexcept = default;
    T& operator*() { return *p_; }
    const T& operator*() const { return *p_; }
    T* operator->() { return p_.get(); }
    const T* operator->() const { return p_.get(); }
    T* get() { return p_.get(); }
    const T* get() const { return p_.get(); }

  private:
    std::unique_ptr<T> p_;
};

}  // namespace touhou::th07

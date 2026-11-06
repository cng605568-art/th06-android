#pragma once

// A fixed-size array kept on the heap (the object pools are too big for the stack) that
// copies by value, so the managers holding them can be copied whole (practice save states).

#include <array>
#include <cstddef>
#include <memory>

namespace touhou {

template <class T, std::size_t N>
class HeapArray {
  public:
    HeapArray() : p_(std::make_unique<std::array<T, N>>()) {}
    HeapArray(const HeapArray& o) : p_(std::make_unique<std::array<T, N>>(*o.p_)) {}
    HeapArray& operator=(const HeapArray& o) {
        if (this != &o) *p_ = *o.p_;  // the storage stays where it is
        return *this;
    }
    HeapArray(HeapArray&&) noexcept = default;
    HeapArray& operator=(HeapArray&&) noexcept = default;

    std::array<T, N>& operator*() { return *p_; }
    const std::array<T, N>& operator*() const { return *p_; }
    std::array<T, N>* operator->() { return p_.get(); }
    const std::array<T, N>* operator->() const { return p_.get(); }

  private:
    std::unique_ptr<std::array<T, N>> p_;
};

}  // namespace touhou

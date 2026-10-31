#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace th06 {

// Fixed-capacity object pool for short-lived game objects (bullets, shots, items).
//
// Allocation: all storage is a member array, so the pool is exactly one allocation for
// its whole lifetime (allocate the owner once, e.g. std::make_unique at startup).
// Spawning and killing thousands of bullets per second never touches the heap, which is
// what keeps a long session from fragmenting it.
//
// Layout: live objects are packed densely in [0, size()). Per-tick update and render
// loops stream through contiguous memory with no holes and no "alive" branches.
//
// Release: the last live object is moved into the hole (swap-remove), O(1). Objects
// therefore move; code that must refer to one object across ticks keeps a Handle
// (slot + generation). A Handle to a released object goes stale and is detected by
// the generation check, even after its slot has been reused.
//
// Iteration rules: forEach() may spawn (new objects are not visited this pass) but must
// not release; use removeIf() to cull while iterating.
template <class T, std::size_t Capacity>
    requires std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>
class ObjectPool {
    static_assert(Capacity > 0 && Capacity < 0x7fffffffu, "unsupported pool capacity");
    static constexpr std::uint32_t kNone = 0xffffffffu;

  public:
    struct Handle {
        std::uint32_t slot = kNone;
        std::uint32_t generation = 0;

        constexpr bool valid() const { return slot != kNone; }
        friend constexpr bool operator==(const Handle&, const Handle&) = default;
    };

    ObjectPool() noexcept { resetFreeList(); }
    ~ObjectPool() { destroyAll(); }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    static constexpr std::size_t capacity() noexcept { return Capacity; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    bool full() const noexcept { return size_ == Capacity; }

    // Constructs an object in place. Returns nullptr when the pool is full; the caller
    // decides what that means (original-game behaviour for bullets is "drop the spawn").
    template <class... Args>
        requires std::constructible_from<T, Args...>
    T* spawn(Args&&... args) {
        return spawnImpl(nullptr, std::forward<Args>(args)...);
    }

    // Same as spawn(), also returning a handle that stays valid until release.
    template <class... Args>
        requires std::constructible_from<T, Args...>
    T* spawnTracked(Handle& handle, Args&&... args) {
        return spawnImpl(&handle, std::forward<Args>(args)...);
    }

    bool alive(Handle h) const noexcept {
        return h.slot < Capacity && generation_[h.slot] == h.generation &&
               slotToDense_[h.slot] != kNone;
    }

    T* get(Handle h) noexcept { return alive(h) ? at(slotToDense_[h.slot]) : nullptr; }
    const T* get(Handle h) const noexcept {
        return alive(h) ? at(slotToDense_[h.slot]) : nullptr;
    }

    // Handle of the object currently at dense position i (0 <= i < size()).
    Handle handleAt(std::size_t i) const noexcept {
        assert(i < size_);
        const std::uint32_t slot = denseToSlot_[i];
        return {slot, generation_[slot]};
    }

    bool release(Handle h) noexcept {
        if (!alive(h)) return false;
        releaseDense(slotToDense_[h.slot]);
        return true;
    }

    // Removes every object for which pred(obj) is true. Visits each live object exactly
    // once: iterating backwards, swap-remove only ever pulls in already-visited objects.
    template <class Pred>
    std::size_t removeIf(Pred&& pred) {
        std::size_t removed = 0;
        for (std::uint32_t i = size_; i-- > 0;) {
            if (pred(*at(i))) {
                releaseDense(i);
                ++removed;
            }
        }
        return removed;
    }

    template <class Fn>
    void forEach(Fn&& fn) {
        const std::uint32_t n = size_;
#ifndef NDEBUG
        ++iterating_;
#endif
        for (std::uint32_t i = 0; i < n; ++i) fn(*at(i));
#ifndef NDEBUG
        --iterating_;
#endif
    }

    template <class Fn>
    void forEach(Fn&& fn) const {
        for (std::uint32_t i = 0; i < size_; ++i) fn(*at(i));
    }

    // Contiguous view of the live objects (valid until the next spawn/release).
    T* begin() noexcept { return at(0); }
    T* end() noexcept { return at(size_); }
    const T* begin() const noexcept { return at(0); }
    const T* end() const noexcept { return at(size_); }

    void clear() noexcept {
        for (std::uint32_t i = 0; i < size_; ++i) {
            const std::uint32_t slot = denseToSlot_[i];
            std::destroy_at(at(i));
            slotToDense_[slot] = kNone;
            ++generation_[slot];
        }
        size_ = 0;
        resetFreeList();
    }

  private:
    T* at(std::size_t i) noexcept {
        return std::launder(reinterpret_cast<T*>(storage_ + i * sizeof(T)));
    }
    const T* at(std::size_t i) const noexcept {
        return std::launder(reinterpret_cast<const T*>(storage_ + i * sizeof(T)));
    }

    template <class... Args>
    T* spawnImpl(Handle* handle, Args&&... args) {
        if (size_ == Capacity) {
            if (handle) *handle = Handle{};
            return nullptr;
        }
        const std::uint32_t slot = freeSlots_[freeCount_ - 1];
        const std::uint32_t dense = size_;
        T* object = std::construct_at(at(dense), std::forward<Args>(args)...);
        // Commit only after construction succeeded (a throwing constructor leaks nothing).
        --freeCount_;
        denseToSlot_[dense] = slot;
        slotToDense_[slot] = dense;
        ++size_;
        if (handle) *handle = Handle{slot, generation_[slot]};
        return object;
    }

    void releaseDense(std::uint32_t dense) noexcept {
        assert(iterating_ == 0 && "release() inside forEach(); use removeIf()");
        assert(dense < size_);
        const std::uint32_t slot = denseToSlot_[dense];
        const std::uint32_t last = size_ - 1;
        std::destroy_at(at(dense));
        if (dense != last) {
            std::construct_at(at(dense), std::move(*at(last)));
            std::destroy_at(at(last));
            const std::uint32_t movedSlot = denseToSlot_[last];
            denseToSlot_[dense] = movedSlot;
            slotToDense_[movedSlot] = dense;
        }
        slotToDense_[slot] = kNone;
        ++generation_[slot];
        freeSlots_[freeCount_++] = slot;
        size_ = last;
    }

    void destroyAll() noexcept {
        for (std::uint32_t i = 0; i < size_; ++i) std::destroy_at(at(i));
        size_ = 0;
    }

    void resetFreeList() noexcept {
        // Pop order hands out slot 0 first, which keeps debugging output readable.
        for (std::uint32_t i = 0; i < Capacity; ++i) {
            freeSlots_[i] = static_cast<std::uint32_t>(Capacity - 1 - i);
            slotToDense_[i] = kNone;
        }
        freeCount_ = static_cast<std::uint32_t>(Capacity);
    }

    alignas(T) std::byte storage_[sizeof(T) * Capacity];
    std::uint32_t denseToSlot_[Capacity];
    std::uint32_t slotToDense_[Capacity];
    std::uint32_t generation_[Capacity] = {};
    std::uint32_t freeSlots_[Capacity];
    std::uint32_t freeCount_ = 0;
    std::uint32_t size_ = 0;
#ifndef NDEBUG
    int iterating_ = 0;
#endif
};

}  // namespace th06

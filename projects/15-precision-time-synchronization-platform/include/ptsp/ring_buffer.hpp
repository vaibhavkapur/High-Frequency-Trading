#pragma once

#include <array>
#include <cstddef>
#include <stdexcept>

namespace ptsp {

// Fixed-capacity, allocation-free ring buffer for bounded hot-path windows.
template <typename T, std::size_t Capacity>
class BoundedRingBuffer {
    static_assert(Capacity > 0, "BoundedRingBuffer capacity must be > 0");

public:
    static constexpr std::size_t kCapacity = Capacity;

    constexpr void clear() noexcept {
        head_ = 0;
        size_ = 0;
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr std::size_t capacity() const noexcept { return Capacity; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr bool full() const noexcept { return size_ == Capacity; }

    // Overwrites oldest element when full (sliding window semantics).
    void push_overwrite(const T& value) noexcept {
        const std::size_t write_idx = (head_ + size_) % Capacity;
        if (size_ == Capacity) {
            storage_[head_] = value;
            head_ = (head_ + 1) % Capacity;
        } else {
            storage_[write_idx] = value;
            ++size_;
        }
    }

    // Strict bounded push: returns false without overwriting if full.
    [[nodiscard]] bool try_push(const T& value) noexcept {
        if (size_ == Capacity) {
            return false;
        }
        const std::size_t write_idx = (head_ + size_) % Capacity;
        storage_[write_idx] = value;
        ++size_;
        return true;
    }

    [[nodiscard]] bool try_pop_front(T& out) noexcept {
        if (size_ == 0) {
            return false;
        }
        out = storage_[head_];
        head_ = (head_ + 1) % Capacity;
        --size_;
        return true;
    }

    [[nodiscard]] const T& operator[](std::size_t logical_index) const noexcept {
        return storage_[(head_ + logical_index) % Capacity];
    }

    [[nodiscard]] const T& newest() const noexcept {
        return storage_[(head_ + size_ - 1) % Capacity];
    }

    [[nodiscard]] const T& oldest() const noexcept {
        return storage_[head_];
    }

private:
    std::array<T, Capacity> storage_{};
    std::size_t head_{0};
    std::size_t size_{0};
};

}  // namespace ptsp

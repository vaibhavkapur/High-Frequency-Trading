#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <vector>

namespace kbnl {

// Bounded lock-free single-producer single-consumer (SPSC) ring buffer.
// Ordering contract:
//   - Producer writes slot then stores tail_ with std::memory_order_release.
//   - Consumer loads tail_ with std::memory_order_acquire, reads slot, then stores head_ with std::memory_order_release.
//   - Capacity must be a power of two >= 2. Usable slots = capacity - 1 (or full power-of-two with monotonic indices).
//   - This implementation uses monotonic 64-bit head_/tail_ counters so all `capacity` slots are usable.
template <typename T>
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(size_t capacity)
        : capacity_(capacity),
          mask_(capacity - 1),
          storage_(capacity) {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("SpscRingBuffer capacity must be a power of two >= 2");
        }
    }

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    bool try_push(const T& item) noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);
        if (tail - head >= capacity_) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        storage_[static_cast<size_t>(tail & mask_)] = item;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool try_push(T&& item) noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);
        if (tail - head >= capacity_) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        storage_[static_cast<size_t>(tail & mask_)] = std::move(item);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head == tail) {
            return false;
        }
        out = std::move(storage_[static_cast<size_t>(head & mask_)]);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    size_t capacity() const noexcept { return capacity_; }

    size_t size_approx() const noexcept {
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        const uint64_t head = head_.load(std::memory_order_acquire);
        return static_cast<size_t>(tail - head);
    }

    bool empty_approx() const noexcept { return size_approx() == 0; }

    uint64_t dropped_count() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    static constexpr size_t kCacheLine = 64;

    const size_t capacity_;
    const size_t mask_;
    std::vector<T> storage_;

    alignas(kCacheLine) std::atomic<uint64_t> tail_{0};
    alignas(kCacheLine) std::atomic<uint64_t> head_{0};
    alignas(kCacheLine) std::atomic<uint64_t> dropped_{0};
};

} // namespace kbnl

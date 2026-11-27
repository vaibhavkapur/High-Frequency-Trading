#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "numa_engine/clock.hpp"
#include "numa_engine/memory.hpp"
#include "numa_engine/topology.hpp"
#include "numa_engine/types.hpp"

namespace numa_engine {

struct QueueBackpressureStats {
    std::uint64_t enqueued_total = 0;
    std::uint64_t dequeued_total = 0;
    std::uint64_t producer_full_spins = 0;
    std::uint64_t consumer_empty_spins = 0;
    std::size_t max_observed_occupancy = 0;
};

// Bounded, lock-free, wait-free-fast-path single-producer single-consumer (SPSC)
// ring buffer backed by NumaBuffer.
//
// Invariants:
// - Capacity is a power of two in [kMinQueueCapacity, kMaxQueueCapacity].
// - Strict FIFO ordering; zero lost events when producer/consumer honor return values.
// - Producer exclusively writes `tail_` and reads `head_`.
// - Consumer exclusively writes `head_` and reads `tail_`.
// - `head_` and `tail_` are placed on distinct 128-byte-aligned cache lines alongside
//   each side's cached snapshot of the peer index to prevent false sharing and reduce
//   inter-core cache-coherency traffic.
// - Memory ordering:
//     * Producer writes slot payload, then stores `tail_` with memory_order_release.
//     * Consumer loads `tail_` with memory_order_acquire, reads payload, then stores
//       `head_` with memory_order_release.
//     * Producer loads `head_` with memory_order_acquire before overwriting slots.
template <typename T>
class SpscRingBuffer {
    static_assert(std::is_trivially_copyable_v<T>, "SpscRingBuffer requires trivially copyable T");

public:
    SpscRingBuffer() = default;
    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    // Move is intended only during setup before producer/consumer threads begin accessing the ring.
    SpscRingBuffer(SpscRingBuffer&& other) noexcept
        : buffer_(std::move(other.buffer_)),
          slots_(other.slots_),
          capacity_(other.capacity_),
          mask_(other.mask_),
          page_size_(other.page_size_) {
        producer_.tail.store(other.producer_.tail.load(std::memory_order_relaxed), std::memory_order_relaxed);
        producer_.cached_head = other.producer_.cached_head;
        producer_.enqueued = other.producer_.enqueued;
        producer_.full_spins = other.producer_.full_spins;
        producer_.max_occupancy = other.producer_.max_occupancy;

        consumer_.head.store(other.consumer_.head.load(std::memory_order_relaxed), std::memory_order_relaxed);
        consumer_.cached_tail = other.consumer_.cached_tail;
        consumer_.dequeued = other.consumer_.dequeued;
        consumer_.empty_spins = other.consumer_.empty_spins;

        other.slots_ = nullptr;
        other.capacity_ = 0;
        other.mask_ = 0;
    }

    SpscRingBuffer& operator=(SpscRingBuffer&& other) noexcept {
        if (this != &other) {
            buffer_ = std::move(other.buffer_);
            slots_ = other.slots_;
            capacity_ = other.capacity_;
            mask_ = other.mask_;
            page_size_ = other.page_size_;

            producer_.tail.store(other.producer_.tail.load(std::memory_order_relaxed), std::memory_order_relaxed);
            producer_.cached_head = other.producer_.cached_head;
            producer_.enqueued = other.producer_.enqueued;
            producer_.full_spins = other.producer_.full_spins;
            producer_.max_occupancy = other.producer_.max_occupancy;

            consumer_.head.store(other.consumer_.head.load(std::memory_order_relaxed), std::memory_order_relaxed);
            consumer_.cached_tail = other.consumer_.cached_tail;
            consumer_.dequeued = other.consumer_.dequeued;
            consumer_.empty_spins = other.consumer_.empty_spins;

            other.slots_ = nullptr;
            other.capacity_ = 0;
            other.mask_ = 0;
        }
        return *this;
    }

    [[nodiscard]] static Result<SpscRingBuffer<T>> create(
        std::size_t capacity,
        const AllocationRequest& alloc_req,
        const TopologyInfo& topology) {
        if (!is_power_of_two(capacity) || capacity < kMinQueueCapacity || capacity > kMaxQueueCapacity) {
            return Error{
                ErrorCode::kInvalidQueueCapacity,
                "Queue capacity must be a power of two between " +
                    std::to_string(kMinQueueCapacity) + " and " +
                    std::to_string(kMaxQueueCapacity),
                "queue_capacity",
                0};
        }

        AllocationRequest req = alloc_req;
        req.bytes = capacity * sizeof(T);
        req.alignment = std::max<std::size_t>(alignof(T), kCacheLineSize);

        auto buf_res = NumaBuffer::allocate(req, topology);
        if (!buf_res.ok()) {
            return buf_res.error();
        }

        SpscRingBuffer<T> ring;
        ring.buffer_ = std::move(buf_res.value());
        ring.slots_ = static_cast<T*>(ring.buffer_.data());
        ring.capacity_ = capacity;
        ring.mask_ = capacity - 1;
        ring.page_size_ = topology.page_size_bytes;
        return ring;
    }

    // Must be called from the target thread (after CPU pinning) when
    // defer_first_touch was requested so pages fault on the caller's NUMA node.
    void first_touch_from_current_thread() noexcept {
        if (buffer_.valid()) {
            buffer_.touch_pages(page_size_);
        }
    }

    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::size_t tail = producer_.tail.load(std::memory_order_relaxed);
        const std::size_t next_tail = tail + 1;

        if (next_tail - producer_.cached_head > capacity_) {
            producer_.cached_head = consumer_.head.load(std::memory_order_acquire);
            if (next_tail - producer_.cached_head > capacity_) {
                ++producer_.full_spins;
                return false;
            }
        }

        slots_[tail & mask_] = item;
        producer_.tail.store(next_tail, std::memory_order_release);
        ++producer_.enqueued;

        const std::size_t occ = next_tail - producer_.cached_head;
        if (occ > producer_.max_occupancy) {
            producer_.max_occupancy = occ;
        }
        return true;
    }

    void push_blocking(const T& item) noexcept {
        while (!try_push(item)) {
            MonotonicClock::cpu_relax();
        }
    }

    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t head = consumer_.head.load(std::memory_order_relaxed);
        if (head == consumer_.cached_tail) {
            consumer_.cached_tail = producer_.tail.load(std::memory_order_acquire);
            if (head == consumer_.cached_tail) {
                ++consumer_.empty_spins;
                return false;
            }
        }

        out = slots_[head & mask_];
        consumer_.head.store(head + 1, std::memory_order_release);
        ++consumer_.dequeued;
        return true;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] const AllocationReport& allocation_report() const noexcept {
        return buffer_.report();
    }

    [[nodiscard]] QueueBackpressureStats stats() const noexcept {
        return QueueBackpressureStats{
            producer_.enqueued,
            consumer_.dequeued,
            producer_.full_spins,
            consumer_.empty_spins,
            producer_.max_occupancy};
    }

private:
    struct alignas(kCacheLineSize) ProducerState {
        std::atomic<std::size_t> tail{0};
        std::size_t cached_head = 0;
        std::uint64_t enqueued = 0;
        std::uint64_t full_spins = 0;
        std::size_t max_occupancy = 0;
    };

    struct alignas(kCacheLineSize) ConsumerState {
        std::atomic<std::size_t> head{0};
        std::size_t cached_tail = 0;
        std::uint64_t dequeued = 0;
        std::uint64_t empty_spins = 0;
    };

    NumaBuffer buffer_{};
    T* slots_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t mask_ = 0;
    std::size_t page_size_ = 4096;

    ProducerState producer_{};
    ConsumerState consumer_{};
};

}  // namespace numa_engine

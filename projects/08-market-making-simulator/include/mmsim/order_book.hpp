#pragma once

#include "mmsim/config.hpp"
#include "mmsim/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace mmsim {

inline constexpr size_t kMaxFeatureDepth = 16;

struct PriceLevel {
    PriceTicks price_ticks = kInvalidPrice;
    Qty qty = 0;
};

class IL2Book {
public:
    virtual ~IL2Book() = default;

    virtual void clear() = 0;

    // Applies an absolute level update (new_qty == 0 removes the level).
    // Returns the previous quantity at (side, price_ticks).
    virtual Qty apply_delta(Side side, PriceTicks price_ticks, Qty new_qty) = 0;

    [[nodiscard]] virtual TopOfBook top_of_book() const noexcept = 0;
    [[nodiscard]] virtual Qty level_qty(Side side, PriceTicks price_ticks) const noexcept = 0;

    // Writes up to `max_levels` best price levels into `out` and returns the count written.
    virtual size_t top_levels(Side side, size_t max_levels,
                              std::array<PriceLevel, kMaxFeatureDepth>& out) const noexcept = 0;
};

// Cache-friendly contiguous tick-array L2 book accelerated by a 2-level 64-bit occupancy bitset
// (using C++20 <bit> countl_zero/countr_zero). Provides O(1) level updates, O(1) best bid/ask
// updates, and O(K) top-K level extraction with zero hot-path allocations.
class FlatArrayBook final : public IL2Book {
public:
    explicit FlatArrayBook(size_t window_ticks = 4096);

    void clear() override;
    Qty apply_delta(Side side, PriceTicks price_ticks, Qty new_qty) override;
    [[nodiscard]] TopOfBook top_of_book() const noexcept override;
    [[nodiscard]] Qty level_qty(Side side, PriceTicks price_ticks) const noexcept override;
    size_t top_levels(Side side, size_t max_levels,
                      std::array<PriceLevel, kMaxFeatureDepth>& out) const noexcept override;

private:
    void init_anchor(PriceTicks anchor_price);
    void recenter(PriceTicks new_center);
    void set_bit(Side side, size_t idx) noexcept;
    void clear_bit(Side side, size_t idx) noexcept;
    [[nodiscard]] int find_highest_bid_below(int idx) const noexcept;
    [[nodiscard]] int find_lowest_ask_above(int idx) const noexcept;

    size_t window_size_ = 4096; // Always a multiple of 64
    size_t num_words_ = 64;
    bool anchored_ = false;
    PriceTicks base_price_ = 0;

    std::vector<Qty> bids_;
    std::vector<Qty> asks_;
    std::vector<uint64_t> bid_words_;
    std::vector<uint64_t> ask_words_;
    std::vector<uint64_t> bid_summary_; // 1 bit per 64 words (covers 4096 ticks per summary word)
    std::vector<uint64_t> ask_summary_;

    int best_bid_idx_ = -1;
    int best_ask_idx_ = -1;
};

// Reference std::map-backed L2 book used for differential correctness testing and latency comparison.
class MapBook final : public IL2Book {
public:
    void clear() override;
    Qty apply_delta(Side side, PriceTicks price_ticks, Qty new_qty) override;
    [[nodiscard]] TopOfBook top_of_book() const noexcept override;
    [[nodiscard]] Qty level_qty(Side side, PriceTicks price_ticks) const noexcept override;
    size_t top_levels(Side side, size_t max_levels,
                      std::array<PriceLevel, kMaxFeatureDepth>& out) const noexcept override;

private:
    std::map<PriceTicks, Qty, std::greater<PriceTicks>> bids_;
    std::map<PriceTicks, Qty> asks_;
};

std::unique_ptr<IL2Book> make_book(BookImplType impl);

} // namespace mmsim

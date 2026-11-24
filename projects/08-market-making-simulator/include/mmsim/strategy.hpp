#pragma once

#include "mmsim/config.hpp"
#include "mmsim/features.hpp"
#include "mmsim/types.hpp"

#include <memory>

namespace mmsim {

struct StrategyState {
    Qty position = 0;
    Qty max_abs_position = 500;
    double maker_fee_bps = 0.0;
};

struct QuoteDecision {
    bool has_bid = false;
    PriceTicks bid_price_ticks = kInvalidPrice;
    Qty bid_qty = 0;

    bool has_ask = false;
    PriceTicks ask_price_ticks = kInvalidPrice;
    Qty ask_qty = 0;

    // Diagnostics for research/logging.
    double fair_value_ticks = 0.0;
    double reservation_price_ticks = 0.0;
    double half_spread_ticks = 0.0;
};

class IStrategy {
public:
    virtual ~IStrategy() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual QuoteDecision compute_quotes(const BookSignals& signals,
                                                       const StrategyState& state) const = 0;
};

// Baseline: quotes symmetrically around mid with fixed spread and zero inventory skew.
class NaiveSymmetricStrategy final : public IStrategy {
public:
    explicit NaiveSymmetricStrategy(const StrategyConfig& cfg);

    [[nodiscard]] std::string name() const override { return "naive_symmetric"; }
    [[nodiscard]] QuoteDecision compute_quotes(const BookSignals& signals,
                                               const StrategyState& state) const override;

private:
    StrategyConfig cfg_{};
};

// Inventory-aware market maker:
//   fair_value        = microprice + alpha_obi * topk_imbalance * (spread/2) + alpha_ofi * ofi
//   reservation_price = fair_value - inventory_penalty(pos, vol)
//   half_spread       = clamp(base + vol_mult * sigma + fee_buffer, min, max)
class InventoryAwareStrategy final : public IStrategy {
public:
    explicit InventoryAwareStrategy(const StrategyConfig& cfg);

    [[nodiscard]] std::string name() const override { return "inventory_aware"; }
    [[nodiscard]] QuoteDecision compute_quotes(const BookSignals& signals,
                                               const StrategyState& state) const override;

private:
    StrategyConfig cfg_{};
};

std::unique_ptr<IStrategy> make_strategy(const StrategyConfig& cfg);

} // namespace mmsim

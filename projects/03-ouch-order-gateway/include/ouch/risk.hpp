// Pre-trade risk checks. Pure integer arithmetic, no allocation on the check path
// beyond a hash lookup for the symbol's reference price.
#pragma once

#include <cstdint>
#include <unordered_map>

#include "ouch/order_manager.hpp"
#include "ouch/types.hpp"

namespace ouch {

struct RiskLimits {
  std::uint32_t max_order_qty = 10'000;
  // Price * shares, in price units (1/10000 dollar). Default: $1,000,000.
  std::uint64_t max_notional = 1'000'000ULL * kPriceScale;
  // Allowed distance from the symbol's reference price, in basis points.
  std::uint32_t price_band_bps = 1'000;
  // If true, an order for a symbol with no reference price is refused.
  bool require_reference_price = true;
  bool allow_market_orders = false;
  // Worst-case position bounds per symbol, counting open orders as if filled.
  std::int64_t max_long = 100'000;
  std::int64_t max_short = 100'000;
};

enum class RiskReject : std::uint8_t {
  None = 0,
  ZeroQuantity,
  MaxOrderQty,
  MaxNotional,
  NoReferencePrice,
  PriceBand,
  MarketOrderNotAllowed,
  PositionLimit,
};

const char* to_string(RiskReject r);

class RiskEngine {
 public:
  explicit RiskEngine(RiskLimits limits = {}) : limits_(limits) {}

  void set_limits(const RiskLimits& l) { limits_ = l; }
  [[nodiscard]] const RiskLimits& limits() const { return limits_; }
  void set_reference_price(const Symbol& s, std::uint32_t price) { ref_[s] = price; }
  [[nodiscard]] std::uint32_t reference_price(const Symbol& s) const;

  // `exposure` is the symbol's current position and open quantity, excluding
  // the order being checked.
  [[nodiscard]] RiskReject check(char side, const Symbol& symbol, std::uint32_t shares,
                                 std::uint32_t price, const SymbolExposure& exposure) const;

 private:
  RiskLimits limits_;
  std::unordered_map<Symbol, std::uint32_t, SymbolHash> ref_;
};

}  // namespace ouch

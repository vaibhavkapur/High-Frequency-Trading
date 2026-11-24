#include "ouch/risk.hpp"

namespace ouch {

const char* to_string(RiskReject r) {
  switch (r) {
    case RiskReject::None:
      return "none";
    case RiskReject::ZeroQuantity:
      return "zero-quantity";
    case RiskReject::MaxOrderQty:
      return "max-order-qty";
    case RiskReject::MaxNotional:
      return "max-notional";
    case RiskReject::NoReferencePrice:
      return "no-reference-price";
    case RiskReject::PriceBand:
      return "price-band";
    case RiskReject::MarketOrderNotAllowed:
      return "market-order-not-allowed";
    case RiskReject::PositionLimit:
      return "position-limit";
  }
  return "?";
}

std::uint32_t RiskEngine::reference_price(const Symbol& s) const {
  auto it = ref_.find(s);
  return it == ref_.end() ? 0 : it->second;
}

RiskReject RiskEngine::check(char side, const Symbol& symbol, std::uint32_t shares,
                             std::uint32_t price, const SymbolExposure& exposure) const {
  if (shares == 0) return RiskReject::ZeroQuantity;
  if (shares > limits_.max_order_qty) return RiskReject::MaxOrderQty;

  const std::uint32_t ref = reference_price(symbol);
  const bool market = price == kMarketPrice;
  if (market && !limits_.allow_market_orders) return RiskReject::MarketOrderNotAllowed;
  if (ref == 0 && (limits_.require_reference_price || market)) {
    return RiskReject::NoReferencePrice;
  }

  // A market order has no limit price; value it at the reference price.
  const std::uint64_t px = market ? ref : price;
  if (px * shares > limits_.max_notional) return RiskReject::MaxNotional;

  if (!market && ref != 0) {
    const std::uint64_t band = static_cast<std::uint64_t>(ref) * limits_.price_band_bps / 10'000;
    const std::uint64_t lo = ref > band ? ref - band : 0;
    const std::uint64_t hi = ref + band;
    if (price < lo || price > hi) return RiskReject::PriceBand;
  }

  if (is_buy(side)) {
    const std::int64_t worst =
        exposure.position + static_cast<std::int64_t>(exposure.open_buy) + shares;
    if (worst > limits_.max_long) return RiskReject::PositionLimit;
  } else {
    const std::int64_t worst =
        exposure.position - static_cast<std::int64_t>(exposure.open_sell) - shares;
    if (worst < -limits_.max_short) return RiskReject::PositionLimit;
  }
  return RiskReject::None;
}

}  // namespace ouch

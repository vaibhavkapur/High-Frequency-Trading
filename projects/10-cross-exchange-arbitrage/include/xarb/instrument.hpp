#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xarb/types.hpp"

namespace xarb {

struct VenueInstrumentConfig {
  std::string native_symbol;  // e.g. "BTCUSDT", "BTC-USDT"
  double taker_fee_bps{10.0};
  double maker_fee_bps{5.0};
  // Native minimum tick / lot increments expressed in canonical ticks/lots (>= 1).
  PriceTicks tick_step{1};
  QtyLots lot_step{1};
  QtyLots min_order_lots{1};
  // Modeling non-instantaneous rebalancing / on-chain transfer costs.
  double withdrawal_fee_base{0.0005};
  TimestampNs transfer_delay_ns{600LL * kNsPerSec};  // 10 min default on-chain confirmation
};

struct InstrumentSpec {
  uint32_t id{0};
  std::string canonical_symbol;  // e.g. "BTC/USDT"
  std::string base_asset;        // e.g. "BTC"
  std::string quote_asset;       // e.g. "USDT"
  int price_decimals{2};         // 1 tick = 10^(-price_decimals) quote
  int qty_decimals{6};           // 1 lot  = 10^(-qty_decimals) base
  double tick_size{0.01};        // cached 10^(-price_decimals)
  double lot_size{1e-6};         // cached 10^(-qty_decimals)
  std::array<VenueInstrumentConfig, kVenueCount> venues{};
  std::array<bool, kVenueCount> venue_enabled{false, false, false};

  double ticks_to_price(PriceTicks ticks) const noexcept { return static_cast<double>(ticks) * tick_size; }
  double lots_to_qty(QtyLots lots) const noexcept { return static_cast<double>(lots) * lot_size; }
  double notional_quote(PriceTicks ticks, QtyLots lots) const noexcept {
    return ticks_to_price(ticks) * lots_to_qty(lots);
  }

  std::optional<PriceTicks> normalize_price(VenueId v, std::string_view raw_price) const noexcept {
    auto parsed = parse_decimal_scaled(raw_price, price_decimals, true);
    if (!parsed || *parsed < 0) return std::nullopt;
    const PriceTicks step = venues[static_cast<size_t>(v)].tick_step;
    if (step > 1) {
      // Snap to venue tick grid.
      return (*parsed / step) * step;
    }
    return *parsed;
  }

  std::optional<QtyLots> normalize_qty(VenueId v, std::string_view raw_qty) const noexcept {
    auto parsed = parse_decimal_scaled(raw_qty, qty_decimals, false);
    if (!parsed || *parsed < 0) return std::nullopt;
    const QtyLots step = venues[static_cast<size_t>(v)].lot_step;
    if (step > 1) {
      return (*parsed / step) * step;
    }
    return *parsed;
  }
};

class InstrumentRegistry {
 public:
  uint32_t register_instrument(InstrumentSpec spec) {
    spec.id = static_cast<uint32_t>(specs_.size());
    spec.tick_size = std::pow(10.0, -spec.price_decimals);
    spec.lot_size = std::pow(10.0, -spec.qty_decimals);
    by_canonical_[spec.canonical_symbol] = spec.id;
    for (size_t i = 0; i < kVenueCount; ++i) {
      if (spec.venue_enabled[i] && !spec.venues[i].native_symbol.empty()) {
        by_venue_symbol_[i][spec.venues[i].native_symbol] = spec.id;
      }
    }
    specs_.push_back(std::move(spec));
    return specs_.back().id;
  }

  const InstrumentSpec* get(uint32_t id) const noexcept {
    return id < specs_.size() ? &specs_[id] : nullptr;
  }

  std::optional<uint32_t> find_canonical(std::string_view sym) const {
    auto it = by_canonical_.find(std::string(sym));
    if (it == by_canonical_.end()) return std::nullopt;
    return it->second;
  }

  std::optional<uint32_t> find_native(VenueId v, std::string_view native_sym) const {
    const auto& map = by_venue_symbol_[static_cast<size_t>(v)];
    auto it = map.find(std::string(native_sym));
    if (it == map.end()) return std::nullopt;
    return it->second;
  }

  size_t size() const noexcept { return specs_.size(); }
  const std::vector<InstrumentSpec>& all() const noexcept { return specs_; }

  static InstrumentRegistry make_default_crypto_registry() {
    InstrumentRegistry reg;

    InstrumentSpec btc;
    btc.canonical_symbol = "BTC/USDT";
    btc.base_asset = "BTC";
    btc.quote_asset = "USDT";
    btc.price_decimals = 2;
    btc.qty_decimals = 6;
    btc.venue_enabled = {true, true, true};
    btc.venues[static_cast<size_t>(VenueId::Binance)] = {"BTCUSDT", 10.0, 10.0, 1, 10, 100, 0.0005, 600LL * kNsPerSec};
    btc.venues[static_cast<size_t>(VenueId::Coinbase)] = {"BTC-USDT", 12.0, 6.0, 1, 10, 100, 0.0004, 600LL * kNsPerSec};
    btc.venues[static_cast<size_t>(VenueId::Okx)] = {"BTC-USDT", 8.0, 5.0, 1, 10, 100, 0.0004, 600LL * kNsPerSec};
    reg.register_instrument(std::move(btc));

    InstrumentSpec eth;
    eth.canonical_symbol = "ETH/USDT";
    eth.base_asset = "ETH";
    eth.quote_asset = "USDT";
    eth.price_decimals = 2;
    eth.qty_decimals = 6;
    eth.venue_enabled = {true, true, true};
    eth.venues[static_cast<size_t>(VenueId::Binance)] = {"ETHUSDT", 10.0, 10.0, 1, 100, 1000, 0.005, 300LL * kNsPerSec};
    eth.venues[static_cast<size_t>(VenueId::Coinbase)] = {"ETH-USDT", 12.0, 6.0, 1, 100, 1000, 0.004, 300LL * kNsPerSec};
    eth.venues[static_cast<size_t>(VenueId::Okx)] = {"ETH-USDT", 8.0, 5.0, 1, 100, 1000, 0.004, 300LL * kNsPerSec};
    reg.register_instrument(std::move(eth));

    return reg;
  }

 private:
  std::vector<InstrumentSpec> specs_;
  std::unordered_map<std::string, uint32_t> by_canonical_;
  std::array<std::unordered_map<std::string, uint32_t>, kVenueCount> by_venue_symbol_;
};

}  // namespace xarb

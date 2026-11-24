#include "me/workload.hpp"

#include <stdexcept>

namespace me {
namespace {

std::uint64_t splitmix(std::uint64_t& state) {
  std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

// Bijective 64-bit mix: distinct counters give distinct ids.
std::uint64_t mix_id(std::uint64_t x) {
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

}  // namespace

Workload::Workload(const WorkloadConfig& config) : cfg_(config), state_(config.seed) {
  if (cfg_.symbols == 0 || cfg_.tick_size <= 0 || cfg_.lot_size <= 0 || cfg_.max_lots == 0 || cfg_.depth_levels == 0) {
    throw std::invalid_argument("workload: symbols, tick, lot, max_lots and depth_levels must be positive");
  }
  if (cfg_.mid_ticks <= static_cast<Price>(cfg_.depth_levels) + 1) {
    throw std::invalid_argument("workload: mid_ticks must exceed depth_levels + 1");
  }
  if (cfg_.add_pm + cfg_.cancel_pm + cfg_.reduce_pm + cfg_.replace_pm != 1000) {
    throw std::invalid_argument("workload: add/cancel/reduce/replace weights must sum to 1000");
  }
  live_.reserve(cfg_.target_live + 1024u);
}

std::uint64_t Workload::rand() {
  return splitmix(state_);
}

std::uint64_t Workload::below(std::uint64_t n) {
  return rand() % n;
}

OrderId Workload::new_id() {
  ++id_counter_;
  return cfg_.random_ids ? mix_id(id_counter_) : id_counter_;
}

Price Workload::passive_price(Side side) {
  const Price offset = 1 + static_cast<Price>(below(cfg_.depth_levels));
  return (side == Side::Buy ? cfg_.mid_ticks - offset : cfg_.mid_ticks + offset) * cfg_.tick_size;
}

Price Workload::aggressive_price(Side side) {
  const Price offset = 1 + static_cast<Price>(below(cfg_.depth_levels / 8 + 1));
  return (side == Side::Buy ? cfg_.mid_ticks + offset : cfg_.mid_ticks - offset) * cfg_.tick_size;
}

Command Workload::make_new() {
  Command c;
  c.type = CommandType::New;
  c.symbol = static_cast<SymbolId>(below(cfg_.symbols));
  c.side = below(2) == 0 ? Side::Buy : Side::Sell;
  c.order_id = new_id();
  c.price = below(1000) < cfg_.aggressive_pm ? aggressive_price(c.side) : passive_price(c.side);
  const Qty lots = 1 + static_cast<Qty>(below(cfg_.max_lots));
  c.qty = lots * cfg_.lot_size;
  live_.push_back(Live{c.symbol, c.order_id, lots});
  return c;
}

Command Workload::make_invalid() {
  Command c = make_new();
  live_.pop_back();  // this order is never meant to rest
  switch (below(10)) {
    case 0:
      c.qty = 0;
      break;
    case 1:
      c.qty = -c.qty;
      break;
    case 2:
      c.price += 1;  // off-tick whenever tick_size > 1
      break;
    case 3:
      c.price = 0;
      break;
    case 4:
      c.price = -c.price;
      break;
    case 5:
      c.symbol = cfg_.symbols + static_cast<SymbolId>(below(3));
      break;
    case 6:
      c.type = static_cast<CommandType>(below(2) == 0 ? 0 : 200);
      break;
    case 7:
      c.side = static_cast<Side>(2 + below(250));
      break;
    case 8:
      c.qty += 1;  // not a lot multiple whenever lot_size > 1
      break;
    default:
      if (!live_.empty()) {  // duplicate of an id that may still be resting
        const Live& l = live_[below(live_.size())];
        c.symbol = l.symbol;
        c.order_id = l.id;
      } else {
        c.qty = 0;
      }
      break;
  }
  return c;
}

Command Workload::next() {
  if (cfg_.invalid_pm != 0 && below(1000) < cfg_.invalid_pm) return make_invalid();
  if (live_.empty()) return make_new();

  const bool above_target = live_.size() >= cfg_.target_live;
  const std::uint32_t add_pm = above_target ? cfg_.cancel_pm : cfg_.add_pm;
  const std::uint32_t cancel_pm = above_target ? cfg_.add_pm : cfg_.cancel_pm;
  const std::uint64_t roll = below(1000);
  if (roll < add_pm) return make_new();

  const std::size_t pick = below(live_.size());
  Live& target = live_[pick];
  Command c;
  c.symbol = target.symbol;
  c.order_id = target.id;
  if (roll < add_pm + cancel_pm) {
    c.type = CommandType::Cancel;
    live_[pick] = live_.back();
    live_.pop_back();
  } else if (roll < add_pm + cancel_pm + cfg_.reduce_pm) {
    c.type = CommandType::Reduce;
    if (target.lots > 1) target.lots = 1 + static_cast<Qty>(below(static_cast<std::uint64_t>(target.lots - 1)));
    c.qty = target.lots * cfg_.lot_size;  // a one-lot order cannot shrink: the engine rejects it
  } else {
    c.type = CommandType::Replace;
    c.new_order_id = new_id();
    const Side side = below(2) == 0 ? Side::Buy : Side::Sell;  // price drawn for either side; may cross
    c.price = passive_price(side);
    target.lots = 1 + static_cast<Qty>(below(cfg_.max_lots));
    c.qty = target.lots * cfg_.lot_size;
    target.id = c.new_order_id;
  }
  return c;
}

std::vector<Command> Workload::generate(std::size_t count) {
  std::vector<Command> out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) out.push_back(next());
  return out;
}

}  // namespace me

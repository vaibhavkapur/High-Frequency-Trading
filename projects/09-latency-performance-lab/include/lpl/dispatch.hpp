#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <variant>
#include <vector>

namespace lpl {

struct TickEvent {
  int64_t price;
  uint32_t qty;
  uint8_t handler_kind;  // 0, 1, or 2
};

inline std::vector<TickEvent> generate_dispatch_events(size_t n, bool heterogeneous,
                                                       uint64_t seed = 9001) {
  std::vector<TickEvent> evs;
  evs.reserve(n);
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> price_dist(9500, 10500);
  std::uniform_int_distribution<uint32_t> qty_dist(1, 200);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t kind = heterogeneous ? static_cast<uint8_t>(rng() % 3ULL) : 0;
    evs.push_back(TickEvent{price_dist(rng), qty_dist(rng), kind});
  }
  return evs;
}

// Identical arithmetic kernels used by all dispatch mechanisms so work is strictly comparable.
struct HandlerState {
  int64_t bias = 0;
  int64_t multiplier = 1;
  uint64_t acc = 0;
};

inline uint64_t compute_alpha_a(HandlerState& st, const TickEvent& ev) noexcept {
  const uint64_t v = static_cast<uint64_t>((ev.price + st.bias) * st.multiplier) + ev.qty;
  st.acc += v;
  return v;
}

inline uint64_t compute_alpha_b(HandlerState& st, const TickEvent& ev) noexcept {
  const uint64_t v =
      (static_cast<uint64_t>(ev.price) * static_cast<uint64_t>(ev.qty)) ^
      static_cast<uint64_t>(st.bias + st.multiplier);
  st.acc += v;
  return v;
}

inline uint64_t compute_alpha_c(HandlerState& st, const TickEvent& ev) noexcept {
  const uint64_t v =
      static_cast<uint64_t>(ev.price - st.bias) + (static_cast<uint64_t>(ev.qty) << 3);
  st.acc += v;
  return v;
}

// 1. Virtual dispatch hierarchy
class VirtualHandler {
 public:
  virtual ~VirtualHandler() = default;
  virtual uint64_t on_tick(const TickEvent& ev) noexcept = 0;
};

class VirtualAlphaA final : public VirtualHandler {
 public:
  explicit VirtualAlphaA(HandlerState st) : st_(st) {}
  uint64_t on_tick(const TickEvent& ev) noexcept override { return compute_alpha_a(st_, ev); }

 private:
  HandlerState st_;
};

class VirtualAlphaB final : public VirtualHandler {
 public:
  explicit VirtualAlphaB(HandlerState st) : st_(st) {}
  uint64_t on_tick(const TickEvent& ev) noexcept override { return compute_alpha_b(st_, ev); }

 private:
  HandlerState st_;
};

class VirtualAlphaC final : public VirtualHandler {
 public:
  explicit VirtualAlphaC(HandlerState st) : st_(st) {}
  uint64_t on_tick(const TickEvent& ev) noexcept override { return compute_alpha_c(st_, ev); }

 private:
  HandlerState st_;
};

__attribute__((noinline)) inline uint64_t run_virtual_dispatch(
    const std::vector<TickEvent>& events, const std::vector<VirtualHandler*>& per_event_handlers) {
  uint64_t total = 0;
  const size_t n = events.size();
  for (size_t i = 0; i < n; ++i) {
    total += per_event_handlers[i]->on_tick(events[i]);
  }
  return total;
}

// 2. Function pointer dispatch
struct FnPtrHandler {
  using Fn = uint64_t (*)(HandlerState&, const TickEvent&) noexcept;
  Fn fn = nullptr;
  HandlerState* state = nullptr;
};

__attribute__((noinline)) inline uint64_t run_fnptr_dispatch(
    const std::vector<TickEvent>& events, const std::vector<FnPtrHandler>& per_event_handlers) {
  uint64_t total = 0;
  const size_t n = events.size();
  for (size_t i = 0; i < n; ++i) {
    total += per_event_handlers[i].fn(*per_event_handlers[i].state, events[i]);
  }
  return total;
}

// 3. std::variant + std::visit closed-set dispatch
struct StaticAlphaA {
  HandlerState st;
  uint64_t on_tick(const TickEvent& ev) noexcept { return compute_alpha_a(st, ev); }
};

struct StaticAlphaB {
  HandlerState st;
  uint64_t on_tick(const TickEvent& ev) noexcept { return compute_alpha_b(st, ev); }
};

struct StaticAlphaC {
  HandlerState st;
  uint64_t on_tick(const TickEvent& ev) noexcept { return compute_alpha_c(st, ev); }
};

using HandlerVariant = std::variant<StaticAlphaA, StaticAlphaB, StaticAlphaC>;

__attribute__((noinline)) inline uint64_t run_variant_dispatch(
    const std::vector<TickEvent>& events, std::vector<HandlerVariant>& handlers) {
  uint64_t total = 0;
  const size_t n = events.size();
  for (size_t i = 0; i < n; ++i) {
    total += std::visit([&](auto& h) noexcept { return h.on_tick(events[i]); },
                        handlers[events[i].handler_kind]);
  }
  return total;
}

// 4. Direct template / static polymorphism (monomorphic, fully inlined)
template <typename HandlerT>
__attribute__((noinline)) inline uint64_t run_template_monomorphic(
    const std::vector<TickEvent>& events, HandlerT& handler) noexcept {
  uint64_t total = 0;
  for (const auto& ev : events) {
    total += handler.on_tick(ev);
  }
  return total;
}

// 5. Direct switch-tag static dispatch (heterogeneous, inlined switch table)
__attribute__((noinline)) inline uint64_t run_switch_static_dispatch(
    const std::vector<TickEvent>& events, StaticAlphaA& a, StaticAlphaB& b,
    StaticAlphaC& c) noexcept {
  uint64_t total = 0;
  for (const auto& ev : events) {
    switch (ev.handler_kind) {
      case 0:
        total += a.on_tick(ev);
        break;
      case 1:
        total += b.on_tick(ev);
        break;
      default:
        total += c.on_tick(ev);
        break;
    }
  }
  return total;
}

}  // namespace lpl

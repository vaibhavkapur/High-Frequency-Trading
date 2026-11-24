#include "itch/synth.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

#include "itch/encoder.hpp"
#include "itch/messages.hpp"

namespace itch {

namespace {

// xoshiro256** seeded through splitmix64. Fully specified, so output does not
// depend on the standard library implementation.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept {
    for (auto& s : s_) {
      seed += 0x9E3779B97F4A7C15ull;
      std::uint64_t z = seed;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      s = z ^ (z >> 31);
    }
  }

  std::uint64_t next() noexcept {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }

  // Uniform in [0, n). Modulo bias is irrelevant at these ranges.
  std::uint64_t below(std::uint64_t n) noexcept { return next() % n; }
  std::uint32_t below32(std::uint32_t n) noexcept { return static_cast<std::uint32_t>(next() % n); }

 private:
  static std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
  std::array<std::uint64_t, 4> s_{};
};

enum Op : std::size_t {
  kAdd,
  kAddMpid,
  kExecute,
  kExecutePrice,
  kCancel,
  kDelete,
  kReplace,
  kTrade,
  kCross,
  kNoii,
  kAdmin,
  kOpCount
};

// Relative weights per profile, summing to 1000.
using Weights = std::array<std::uint32_t, kOpCount>;

constexpr Weights weights_for(SynthProfile p) noexcept {
  switch (p) {
    case SynthProfile::Balanced:
      //       A   F   E   C   X    D   U   P  Q   I  admin
      return {400, 30, 80, 10, 80, 260, 90, 30, 2, 10, 8};
    case SynthProfile::AddHeavy:
      return {850, 50, 10, 0, 10, 50, 20, 10, 0, 0, 0};
    case SynthProfile::Churn:
      return {460, 0, 10, 0, 20, 460, 50, 0, 0, 0, 0};
  }
  return {};
}

struct Live {
  OrderRef ref;
  std::uint32_t qty;
  Price4 price;
  std::uint16_t locate;
  Side side;
};

struct SymbolInfo {
  Symbol name;
  Price4 mid;
};

constexpr Price4 kTick = 100;  // $0.01
constexpr std::uint32_t kMaxOffsetTicks = 50;
constexpr Timestamp kNsPerSecond = 1'000'000'000ull;
constexpr Timestamp kOpen = 34'200 * kNsPerSecond;     // 09:30:00
constexpr Timestamp kSession = 23'400 * kNsPerSecond;  // 6.5 hours

class Generator {
 public:
  explicit Generator(const SynthConfig& cfg)
      : cfg_(cfg), rng_(cfg.seed), weights_(weights_for(cfg.profile)) {
    const std::uint32_t n = std::clamp<std::uint32_t>(cfg.symbols, 1, 65'535);
    symbols_.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      char name[9];
      std::snprintf(name, sizeof name, "S%05u", i + 1);
      // Mid between $10.00 and $499.99, on the tick grid.
      const Price4 mid = (1'000 + rng_.below32(49'000)) * kTick;
      symbols_.push_back({Symbol::from(name), mid});
    }
    step_ = std::max<Timestamp>(1, kSession / std::max<std::uint64_t>(1, cfg.messages));
    out_.reserve(static_cast<std::size_t>(cfg.messages) * 36 + symbols_.size() * 70 + 256);
  }

  std::vector<std::uint8_t> run() {
    preamble();
    // Churn models quoting against a standing book, so build one first;
    // otherwise equal add/delete rates would leave the book almost empty.
    std::uint64_t warmup = 0;
    if (cfg_.profile == SynthProfile::Churn) {
      warmup = std::min<std::uint64_t>(cfg_.messages / 5, std::uint64_t{400} * symbols_.size());
    }
    for (std::uint64_t i = 0; i < cfg_.messages; ++i) {
      ts_ += rng_.below(2 * step_);
      if (i < warmup) {
        emit_add(false);
      } else {
        emit_one();
      }
    }
    epilogue();
    return std::move(out_);
  }

 private:
  Header hdr(std::uint16_t locate) noexcept { return {locate, 0, ts_}; }

  std::uint16_t random_locate() noexcept {
    return static_cast<std::uint16_t>(1 +
                                      rng_.below32(static_cast<std::uint32_t>(symbols_.size())));
  }
  const SymbolInfo& sym(std::uint16_t locate) const noexcept { return symbols_[locate - 1u]; }

  // Bids strictly below mid, asks strictly above, so books never cross.
  // min() of two draws skews orders toward the inside of the book.
  Price4 random_price(std::uint16_t locate, Side side) noexcept {
    const std::uint32_t off =
        1 + std::min(rng_.below32(kMaxOffsetTicks), rng_.below32(kMaxOffsetTicks));
    const Price4 mid = sym(locate).mid;
    return side == Side::Buy ? mid - off * kTick : mid + off * kTick;
  }
  std::uint32_t random_shares() noexcept { return 100 * (1 + rng_.below32(10)); }

  void system_event(char code) { encode_framed(SystemEvent{hdr(0), code}, out_); }

  void preamble() {
    ts_ = kOpen - 60 * kNsPerSecond;
    system_event('O');
    for (std::size_t i = 0; i < symbols_.size(); ++i) {
      const auto locate = static_cast<std::uint16_t>(i + 1);
      StockDirectory d;
      d.hdr = hdr(locate);
      d.stock = symbols_[i].name;
      d.market_category = 'Q';
      d.financial_status = 'N';
      d.round_lot_size = 100;
      d.round_lots_only = 'N';
      d.issue_classification = 'C';
      d.issue_subtype = {'Z', ' '};
      d.authenticity = 'P';
      d.short_sale_threshold = 'N';
      d.ipo_flag = 'N';
      d.luld_tier = '1';
      d.etp_flag = 'N';
      d.etp_leverage = 0;
      d.inverse_indicator = 'N';
      encode_framed(d, out_);
      encode_framed(
          StockTradingAction{hdr(locate), symbols_[i].name, 'T', ' ', {' ', ' ', ' ', ' '}}, out_);
    }
    system_event('S');
    ts_ = kOpen;
    system_event('Q');
  }

  void epilogue() {
    ts_ += kNsPerSecond;
    system_event('M');
    system_event('E');
    system_event('C');
  }

  Op pick_op() noexcept {
    std::uint32_t r = rng_.below32(1000);
    for (std::size_t i = 0; i < kOpCount; ++i) {
      if (r < weights_[i]) return static_cast<Op>(i);
      r -= weights_[i];
    }
    return kAdd;
  }

  void emit_one() {
    Op op = pick_op();
    const bool needs_order =
        op == kExecute || op == kExecutePrice || op == kCancel || op == kDelete || op == kReplace;
    if (needs_order && live_.empty()) op = kAdd;
    switch (op) {
      case kAdd:
      case kAddMpid:
        emit_add(op == kAddMpid);
        break;
      case kExecute:
      case kExecutePrice:
        emit_execute(op == kExecutePrice);
        break;
      case kCancel:
        emit_cancel();
        break;
      case kDelete:
        emit_delete();
        break;
      case kReplace:
        emit_replace();
        break;
      case kTrade:
        emit_trade();
        break;
      case kCross:
        emit_cross();
        break;
      case kNoii:
        emit_noii();
        break;
      case kAdmin:
      case kOpCount:
        emit_admin();
        break;
    }
  }

  void emit_add(bool mpid) {
    const std::uint16_t locate = random_locate();
    const Side side = rng_.below(2) == 0 ? Side::Buy : Side::Sell;
    const Live o{next_ref_++, random_shares(), random_price(locate, side), locate, side};
    if (mpid) {
      encode_framed(
          AddOrderMpid{
              hdr(locate), o.ref, side, o.qty, sym(locate).name, o.price, {'M', 'M', 'I', 'D'}},
          out_);
    } else {
      encode_framed(AddOrder{hdr(locate), o.ref, side, o.qty, sym(locate).name, o.price}, out_);
    }
    live_.push_back(o);
  }

  std::size_t pick_live() noexcept { return static_cast<std::size_t>(rng_.below(live_.size())); }

  void drop_live(std::size_t i) noexcept {
    live_[i] = live_.back();
    live_.pop_back();
  }

  void emit_execute(bool with_price) {
    const std::size_t i = pick_live();
    Live& o = live_[i];
    // Half of executions fill the order completely.
    const std::uint32_t qty = rng_.below(2) == 0 ? o.qty : 1 + rng_.below32(o.qty);
    if (with_price) {
      const char printable = rng_.below(4) == 0 ? 'N' : 'Y';
      encode_framed(
          OrderExecutedWithPrice{hdr(o.locate), o.ref, qty, next_match_++, printable, o.price},
          out_);
    } else {
      encode_framed(OrderExecuted{hdr(o.locate), o.ref, qty, next_match_++}, out_);
    }
    o.qty -= qty;
    if (o.qty == 0) drop_live(i);
  }

  void emit_cancel() {
    const std::size_t i = pick_live();
    Live& o = live_[i];
    if (o.qty < 2) {
      encode_framed(OrderDelete{hdr(o.locate), o.ref}, out_);
      drop_live(i);
      return;
    }
    const std::uint32_t qty = 1 + rng_.below32(o.qty - 1);  // always partial
    encode_framed(OrderCancel{hdr(o.locate), o.ref, qty}, out_);
    o.qty -= qty;
  }

  void emit_delete() {
    const std::size_t i = pick_live();
    encode_framed(OrderDelete{hdr(live_[i].locate), live_[i].ref}, out_);
    drop_live(i);
  }

  void emit_replace() {
    Live& o = live_[pick_live()];
    const OrderRef fresh = next_ref_++;
    const std::uint32_t qty = random_shares();
    const Price4 price = random_price(o.locate, o.side);
    encode_framed(OrderReplace{hdr(o.locate), o.ref, fresh, qty, price}, out_);
    o.ref = fresh;
    o.qty = qty;
    o.price = price;
  }

  void emit_trade() {
    const std::uint16_t locate = random_locate();
    // Non-displayed executions print at a price inside the spread; the mid is
    // never a resting displayed price in this generator.
    encode_framed(Trade{hdr(locate), 0, Side::Buy, random_shares(), sym(locate).name,
                        sym(locate).mid, next_match_++},
                  out_);
  }

  void emit_cross() {
    const std::uint16_t locate = random_locate();
    encode_framed(CrossTrade{hdr(locate), std::uint64_t{random_shares()} * 50, sym(locate).name,
                             sym(locate).mid, next_match_++, 'O'},
                  out_);
  }

  void emit_noii() {
    const std::uint16_t locate = random_locate();
    const Price4 mid = sym(locate).mid;
    Noii m;
    m.hdr = hdr(locate);
    m.paired_shares = std::uint64_t{random_shares()} * 10;
    m.imbalance_shares = random_shares();
    m.imbalance_direction = rng_.below(2) == 0 ? 'B' : 'S';
    m.stock = sym(locate).name;
    m.far_price = mid - kTick;
    m.near_price = mid + kTick;
    m.current_reference_price = mid;
    m.cross_type = 'C';
    m.price_variation_indicator = 'L';
    encode_framed(m, out_);
  }

  // Cycles through the remaining administrative message types so that a
  // synthetic file exercises the whole supported-message matrix.
  void emit_admin() {
    const std::uint16_t locate = random_locate();
    const Symbol name = sym(locate).name;
    const Price4 mid = sym(locate).mid;
    switch (admin_cycle_++ % 11) {
      case 0:
        encode_framed(StockTradingAction{hdr(locate), name, 'T', ' ', {' ', ' ', ' ', ' '}}, out_);
        break;
      case 1:
        encode_framed(RegShoRestriction{hdr(locate), name, '0'}, out_);
        break;
      case 2:
        encode_framed(
            MarketParticipantPosition{hdr(locate), {'M', 'M', 'I', 'D'}, name, 'Y', 'N', 'A'},
            out_);
        break;
      case 3:
        encode_framed(
            MwcbDeclineLevel{hdr(0), 400'000'000'000ull, 380'000'000'000ull, 350'000'000'000ull},
            out_);
        break;
      case 4:
        encode_framed(MwcbStatus{hdr(0), '1'}, out_);
        break;
      case 5:
        encode_framed(IpoQuotingPeriod{hdr(locate), name, 34'200, 'A', mid}, out_);
        break;
      case 6:
        encode_framed(
            LuldAuctionCollar{hdr(locate), name, mid, mid + 500 * kTick, mid - 500 * kTick, 0},
            out_);
        break;
      case 7:
        encode_framed(OperationalHalt{hdr(locate), name, 'Q', 'T'}, out_);
        break;
      case 8:
        encode_framed(Rpii{hdr(locate), name, 'N'}, out_);
        break;
      case 9:
        encode_framed(BrokenTrade{hdr(locate), 1 + rng_.below(next_match_)}, out_);
        break;
      default: {
        DlcrPriceDiscovery m;
        m.hdr = hdr(locate);
        m.stock = name;
        m.open_eligibility_status = 'N';
        m.min_allowable_price = mid - 100 * kTick;
        m.max_allowable_price = mid + 100 * kTick;
        m.near_execution_price = mid;
        m.near_execution_time = ts_;
        m.lower_price_range_collar = mid - 200 * kTick;
        m.upper_price_range_collar = mid + 200 * kTick;
        encode_framed(m, out_);
        break;
      }
    }
  }

  SynthConfig cfg_;
  Rng rng_;
  Weights weights_;
  std::vector<SymbolInfo> symbols_;
  std::vector<Live> live_;
  std::vector<std::uint8_t> out_;
  Timestamp ts_ = 0;
  Timestamp step_ = 1;
  OrderRef next_ref_ = 1;
  std::uint64_t next_match_ = 1;
  std::uint32_t admin_cycle_ = 0;
};

}  // namespace

bool parse_profile(std::string_view name, SynthProfile& out) noexcept {
  if (name == "balanced") {
    out = SynthProfile::Balanced;
  } else if (name == "add-heavy") {
    out = SynthProfile::AddHeavy;
  } else if (name == "churn") {
    out = SynthProfile::Churn;
  } else {
    return false;
  }
  return true;
}

const char* to_string(SynthProfile p) noexcept {
  switch (p) {
    case SynthProfile::Balanced:
      return "balanced";
    case SynthProfile::AddHeavy:
      return "add-heavy";
    case SynthProfile::Churn:
      return "churn";
  }
  return "?";
}

std::vector<std::uint8_t> generate_synthetic(const SynthConfig& cfg) {
  return Generator{cfg}.run();
}

}  // namespace itch

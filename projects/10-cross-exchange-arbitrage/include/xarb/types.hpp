#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace xarb {

using TimestampNs = int64_t;
using PriceTicks = int64_t;
using QtyLots = int64_t;
using SeqNum = uint64_t;

constexpr int64_t kNsPerUs = 1'000LL;
constexpr int64_t kNsPerMs = 1'000'000LL;
constexpr int64_t kNsPerSec = 1'000'000'000LL;

enum class VenueId : uint8_t {
  Binance = 0,
  Coinbase = 1,
  Okx = 2,
  Count = 3,
};

constexpr size_t kVenueCount = static_cast<size_t>(VenueId::Count);

inline std::string_view venue_name(VenueId v) noexcept {
  switch (v) {
    case VenueId::Binance:
      return "binance";
    case VenueId::Coinbase:
      return "coinbase";
    case VenueId::Okx:
      return "okx";
    case VenueId::Count:
      return "unknown";
  }
  return "unknown";
}

inline std::optional<VenueId> parse_venue_name(std::string_view s) noexcept {
  if (s == "binance") return VenueId::Binance;
  if (s == "coinbase") return VenueId::Coinbase;
  if (s == "okx") return VenueId::Okx;
  return std::nullopt;
}

enum class Side : uint8_t {
  Bid = 0,
  Ask = 1,
};

inline std::string_view side_name(Side s) noexcept {
  return s == Side::Bid ? "bid" : "ask";
}

enum class BookState : uint8_t {
  Disconnected = 0,
  Connecting = 1,
  Buffering = 2,
  Synced = 3,
  Stale = 4,
  Resyncing = 5,
};

inline std::string_view book_state_name(BookState st) noexcept {
  switch (st) {
    case BookState::Disconnected:
      return "disconnected";
    case BookState::Connecting:
      return "connecting";
    case BookState::Buffering:
      return "buffering";
    case BookState::Synced:
      return "synced";
    case BookState::Stale:
      return "stale";
    case BookState::Resyncing:
      return "resyncing";
  }
  return "unknown";
}

struct PriceLevel {
  PriceTicks price{0};
  QtyLots qty{0};
};

// Parse a positive or zero decimal string (e.g. "65123.45") into scaled integer units
// using exact integer arithmetic (no floating-point rounding drift).
// scale_decimals is the number of fractional digits represented by 1 unit (e.g. 2 for cents, 6 for 1e-6 BTC).
// Rounding mode: round half-up (or truncate if round_up == false).
inline std::optional<int64_t> parse_decimal_scaled(std::string_view text, int scale_decimals,
                                                   bool round_nearest = true) noexcept {
  if (text.empty() || scale_decimals < 0 || scale_decimals > 12) {
    return std::nullopt;
  }

  size_t i = 0;
  bool negative = false;
  if (text[i] == '-') {
    negative = true;
    ++i;
  } else if (text[i] == '+') {
    ++i;
  }
  if (i >= text.size()) {
    return std::nullopt;
  }

  uint64_t int_part = 0;
  bool has_digit = false;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    has_digit = true;
    uint64_t d = static_cast<uint64_t>(text[i] - '0');
    if (int_part > (static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - d) / 10ULL) {
      return std::nullopt;
    }
    int_part = int_part * 10ULL + d;
    ++i;
  }

  uint64_t frac_part = 0;
  int frac_digits = 0;
  bool round_bit = false;
  if (i < text.size() && text[i] == '.') {
    ++i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      has_digit = true;
      uint64_t d = static_cast<uint64_t>(text[i] - '0');
      if (frac_digits < scale_decimals) {
        frac_part = frac_part * 10ULL + d;
        ++frac_digits;
      } else if (frac_digits == scale_decimals) {
        if (d >= 5ULL) round_bit = true;
        ++frac_digits;
      }
      ++i;
    }
  }

  if (!has_digit || i != text.size()) {
    return std::nullopt;
  }

  while (frac_digits < scale_decimals) {
    frac_part *= 10ULL;
    ++frac_digits;
  }

  uint64_t scale_mult = 1;
  for (int k = 0; k < scale_decimals; ++k) {
    scale_mult *= 10ULL;
  }

  if (int_part > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / scale_mult) {
    return std::nullopt;
  }
  uint64_t scaled = int_part * scale_mult + frac_part;
  if (round_nearest && round_bit) {
    if (scaled == static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return std::nullopt;
    }
    ++scaled;
  }

  if (scaled > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return std::nullopt;
  }
  int64_t result = static_cast<int64_t>(scaled);
  return negative ? -result : result;
}

inline std::string format_scaled(int64_t value, int scale_decimals) {
  bool neg = value < 0;
  uint64_t abs_val = neg ? static_cast<uint64_t>(-(value + 1)) + 1ULL : static_cast<uint64_t>(value);
  uint64_t mult = 1;
  for (int i = 0; i < scale_decimals; ++i) mult *= 10ULL;
  uint64_t whole = abs_val / mult;
  uint64_t frac = abs_val % mult;
  std::string out;
  if (neg) out.push_back('-');
  out += std::to_string(whole);
  if (scale_decimals > 0) {
    out.push_back('.');
    std::string f = std::to_string(frac);
    if (static_cast<int>(f.size()) < scale_decimals) {
      out.append(static_cast<size_t>(scale_decimals - static_cast<int>(f.size())), '0');
    }
    out += f;
  }
  return out;
}

// 64-bit FNV-1a hash helper for deterministic run digests.
struct Fnv1a64 {
  uint64_t state{14695981039346656037ULL};

  void update_bytes(const void* data, size_t len) noexcept {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
      state ^= static_cast<uint64_t>(p[i]);
      state *= 1099511628211ULL;
    }
  }

  void update_u64(uint64_t v) noexcept { update_bytes(&v, sizeof(v)); }
  void update_i64(int64_t v) noexcept { update_bytes(&v, sizeof(v)); }
  void update_str(std::string_view s) noexcept {
    update_u64(static_cast<uint64_t>(s.size()));
    if (!s.empty()) update_bytes(s.data(), s.size());
  }
};

}  // namespace xarb

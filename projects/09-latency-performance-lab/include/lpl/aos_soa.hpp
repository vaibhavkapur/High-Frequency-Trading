#pragma once

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

namespace lpl {

// Realistic order/quote record with hot fields (price, qty, side) and cold metadata
// (order_id, timestamp, venue, client_tag, padding) totaling 64 bytes per record.
struct alignas(64) OrderRecordAoS {
  int64_t price;        // hot
  uint32_t qty;         // hot
  uint8_t side;         // hot: 0 = bid, 1 = ask
  uint8_t flags;        // cold
  uint16_t symbol_id;   // cold
  uint64_t order_id;    // cold
  uint64_t timestamp;   // cold
  uint64_t account_id;  // cold
  uint64_t client_tag;  // cold
  uint64_t reserved[2]; // cold padding to fill 64-byte record
};

static_assert(sizeof(OrderRecordAoS) == 64, "OrderRecordAoS must be 64 bytes");

struct OrderRecordsSoA {
  std::vector<int64_t> price;
  std::vector<uint32_t> qty;
  std::vector<uint8_t> side;
  std::vector<uint8_t> flags;
  std::vector<uint16_t> symbol_id;
  std::vector<uint64_t> order_id;
  std::vector<uint64_t> timestamp;
  std::vector<uint64_t> account_id;
  std::vector<uint64_t> client_tag;

  size_t size() const noexcept { return price.size(); }

  void resize(size_t n) {
    price.resize(n);
    qty.resize(n);
    side.resize(n);
    flags.resize(n);
    symbol_id.resize(n);
    order_id.resize(n);
    timestamp.resize(n);
    account_id.resize(n);
    client_tag.resize(n);
  }
};

struct AoSSoADataset {
  std::vector<OrderRecordAoS> aos;
  OrderRecordsSoA soa;
  std::vector<uint32_t> random_indices;
};

inline AoSSoADataset make_aos_soa_dataset(size_t n, uint64_t seed = 42) {
  AoSSoADataset ds;
  ds.aos.resize(n);
  ds.soa.resize(n);
  ds.random_indices.resize(n);

  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> price_dist(9900, 10100);
  std::uniform_int_distribution<uint32_t> qty_dist(1, 500);

  for (size_t i = 0; i < n; ++i) {
    const int64_t p = price_dist(rng);
    const uint32_t q = qty_dist(rng);
    const uint8_t s = static_cast<uint8_t>(rng() & 1ULL);
    const uint16_t sym = static_cast<uint16_t>(i & 0xFFU);
    const uint64_t oid = 1'000'000ULL + i;
    const uint64_t ts = 1'700'000'000'000ULL + i * 100ULL;
    const uint64_t acct = (i * 17ULL) & 0xFFFFULL;
    const uint64_t tag = rng();

    ds.aos[i] = OrderRecordAoS{p, q, s, 0, sym, oid, ts, acct, tag, {0, 0}};
    ds.soa.price[i] = p;
    ds.soa.qty[i] = q;
    ds.soa.side[i] = s;
    ds.soa.flags[i] = 0;
    ds.soa.symbol_id[i] = sym;
    ds.soa.order_id[i] = oid;
    ds.soa.timestamp[i] = ts;
    ds.soa.account_id[i] = acct;
    ds.soa.client_tag[i] = tag;
    ds.random_indices[i] = static_cast<uint32_t>(i);
  }

  std::shuffle(ds.random_indices.begin(), ds.random_indices.end(), rng);
  return ds;
}

// Sequential scan: compute bid notional sum (reads only hot fields: price, qty, side).
__attribute__((noinline)) inline uint64_t scan_sequential_aos(
    const std::vector<OrderRecordAoS>& aos) noexcept {
  uint64_t notional = 0;
  const size_t n = aos.size();
  for (size_t i = 0; i < n; ++i) {
    const uint64_t mask = static_cast<uint64_t>(aos[i].side == 0);
    notional += mask * static_cast<uint64_t>(aos[i].price) * static_cast<uint64_t>(aos[i].qty);
  }
  return notional;
}

__attribute__((noinline)) inline uint64_t scan_sequential_soa(const OrderRecordsSoA& soa) noexcept {
  uint64_t notional = 0;
  const size_t n = soa.size();
  const int64_t* __restrict price = soa.price.data();
  const uint32_t* __restrict qty = soa.qty.data();
  const uint8_t* __restrict side = soa.side.data();
  for (size_t i = 0; i < n; ++i) {
    const uint64_t mask = static_cast<uint64_t>(side[i] == 0);
    notional += mask * static_cast<uint64_t>(price[i]) * static_cast<uint64_t>(qty[i]);
  }
  return notional;
}

// Random access (single-field price sum): SoA packs 8 prices per 64B (16 per 128B) cache line.
__attribute__((noinline)) inline uint64_t scan_random_single_field_aos(
    const std::vector<OrderRecordAoS>& aos, const std::vector<uint32_t>& indices) noexcept {
  uint64_t sum = 0;
  for (uint32_t idx : indices) {
    sum += static_cast<uint64_t>(aos[idx].price);
  }
  return sum;
}

__attribute__((noinline)) inline uint64_t scan_random_single_field_soa(
    const OrderRecordsSoA& soa, const std::vector<uint32_t>& indices) noexcept {
  uint64_t sum = 0;
  const int64_t* __restrict price = soa.price.data();
  for (uint32_t idx : indices) {
    sum += static_cast<uint64_t>(price[idx]);
  }
  return sum;
}

// Random access (multi-field: price, qty, side, order_id):
// AoS fetches all 4 fields in 1 cache line miss; SoA touches 4 separate arrays (up to 4 misses)!
__attribute__((noinline)) inline uint64_t scan_random_multi_field_aos(
    const std::vector<OrderRecordAoS>& aos, const std::vector<uint32_t>& indices) noexcept {
  uint64_t acc = 0;
  for (uint32_t idx : indices) {
    const OrderRecordAoS& r = aos[idx];
    acc += static_cast<uint64_t>(r.price) * static_cast<uint64_t>(r.qty) +
           (r.order_id ^ static_cast<uint64_t>(r.side));
  }
  return acc;
}

__attribute__((noinline)) inline uint64_t scan_random_multi_field_soa(
    const OrderRecordsSoA& soa, const std::vector<uint32_t>& indices) noexcept {
  uint64_t acc = 0;
  const int64_t* __restrict price = soa.price.data();
  const uint32_t* __restrict qty = soa.qty.data();
  const uint8_t* __restrict side = soa.side.data();
  const uint64_t* __restrict oid = soa.order_id.data();
  for (uint32_t idx : indices) {
    acc += static_cast<uint64_t>(price[idx]) * static_cast<uint64_t>(qty[idx]) +
           (oid[idx] ^ static_cast<uint64_t>(side[idx]));
  }
  return acc;
}

}  // namespace lpl

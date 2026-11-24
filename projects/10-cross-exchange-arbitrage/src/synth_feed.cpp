#include "xarb/synth_feed.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <sstream>

#include "xarb/crc32.hpp"

namespace xarb {

namespace {

std::string format_iso_utc(TimestampNs ts_ns) {
  int64_t sec = ts_ns / kNsPerSec;
  int64_t us = (ts_ns % kNsPerSec) / 1000LL;
  // Simple civil conversion from epoch seconds
  int64_t z = sec / 86400LL + 719468LL;
  int64_t sod = sec % 86400LL;
  if (sod < 0) {
    sod += 86400LL;
    --z;
  }
  int64_t era = (z >= 0 ? z : z - 146096LL) / 146097LL;
  unsigned doe = static_cast<unsigned>(z - era * 146097LL);
  unsigned yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
  int64_t y = static_cast<int64_t>(yoe) + era * 400LL;
  unsigned doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
  unsigned mp = (5U * doy + 2U) / 153U;
  unsigned d = doy - (153U * mp + 2U) / 5U + 1U;
  unsigned m = mp + (mp < 10U ? 3U : -9U);
  y += (m <= 2U ? 1LL : 0LL);
  unsigned hh = static_cast<unsigned>(sod / 3600LL);
  unsigned mm = static_cast<unsigned>((sod % 3600LL) / 60LL);
  unsigned ss = static_cast<unsigned>(sod % 60LL);

  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02u:%02u:%02u.%06lldZ", static_cast<long long>(y), m, d, hh, mm,
                ss, static_cast<long long>(us));
  return std::string(buf);
}

struct BookSnapshotData {
  std::map<PriceTicks, QtyLots, std::greater<PriceTicks>> bids;
  std::map<PriceTicks, QtyLots, std::less<PriceTicks>> asks;
};

int32_t okx_checksum_from_maps(const BookSnapshotData& b, int price_dec, int qty_dec) {
  std::string payload;
  payload.reserve(1024);
  auto bit = b.bids.begin();
  auto ait = b.asks.begin();
  for (int i = 0; i < 25; ++i) {
    const bool has_b = (bit != b.bids.end());
    const bool has_a = (ait != b.asks.end());
    if (!has_b && !has_a) break;
    if (has_b) {
      if (!payload.empty()) payload.push_back(':');
      payload += format_scaled(bit->first, price_dec);
      payload.push_back(':');
      payload += format_scaled(bit->second, qty_dec);
      ++bit;
    }
    if (has_a) {
      if (!payload.empty()) payload.push_back(':');
      payload += format_scaled(ait->first, price_dec);
      payload.push_back(':');
      payload += format_scaled(ait->second, qty_dec);
      ++ait;
    }
  }
  return Crc32::compute_signed(payload);
}

std::string make_binance_snapshot(const BookSnapshotData& book, SeqNum last_id, int pdec, int qdec) {
  std::ostringstream oss;
  oss << "{\"lastUpdateId\":" << last_id << ",\"bids\":[";
  bool first = true;
  for (const auto& [p, q] : book.bids) {
    if (!first) oss << ',';
    first = false;
    oss << "[\"" << format_scaled(p, pdec) << "\",\"" << format_scaled(q, qdec) << "\"]";
  }
  oss << "],\"asks\":[";
  first = true;
  for (const auto& [p, q] : book.asks) {
    if (!first) oss << ',';
    first = false;
    oss << "[\"" << format_scaled(p, pdec) << "\",\"" << format_scaled(q, qdec) << "\"]";
  }
  oss << "]}";
  return oss.str();
}

std::string make_binance_delta(const std::string& sym, TimestampNs ts_ns, SeqNum first_u, SeqNum final_u,
                               const std::vector<PriceLevel>& bids, const std::vector<PriceLevel>& asks, int pdec,
                               int qdec) {
  std::ostringstream oss;
  oss << "{\"e\":\"depthUpdate\",\"E\":" << (ts_ns / kNsPerMs) << ",\"s\":\"" << sym << "\",\"U\":" << first_u
      << ",\"u\":" << final_u << ",\"b\":[";
  for (size_t i = 0; i < bids.size(); ++i) {
    if (i > 0) oss << ',';
    oss << "[\"" << format_scaled(bids[i].price, pdec) << "\",\"" << format_scaled(bids[i].qty, qdec) << "\"]";
  }
  oss << "],\"a\":[";
  for (size_t i = 0; i < asks.size(); ++i) {
    if (i > 0) oss << ',';
    oss << "[\"" << format_scaled(asks[i].price, pdec) << "\",\"" << format_scaled(asks[i].qty, qdec) << "\"]";
  }
  oss << "]}";
  return oss.str();
}

std::string make_coinbase_snapshot(const std::string& sym, TimestampNs ts_ns, SeqNum seq, const BookSnapshotData& book,
                                   int pdec, int qdec) {
  std::ostringstream oss;
  oss << "{\"type\":\"snapshot\",\"product_id\":\"" << sym << "\",\"sequence\":" << seq << ",\"time\":\""
      << format_iso_utc(ts_ns) << "\",\"bids\":[";
  bool first = true;
  for (const auto& [p, q] : book.bids) {
    if (!first) oss << ',';
    first = false;
    oss << "[\"" << format_scaled(p, pdec) << "\",\"" << format_scaled(q, qdec) << "\"]";
  }
  oss << "],\"asks\":[";
  first = true;
  for (const auto& [p, q] : book.asks) {
    if (!first) oss << ',';
    first = false;
    oss << "[\"" << format_scaled(p, pdec) << "\",\"" << format_scaled(q, qdec) << "\"]";
  }
  oss << "]}";
  return oss.str();
}

std::string make_coinbase_delta(const std::string& sym, TimestampNs ts_ns, SeqNum seq,
                                const std::vector<PriceLevel>& bids, const std::vector<PriceLevel>& asks, int pdec,
                                int qdec) {
  std::ostringstream oss;
  oss << "{\"type\":\"l2update\",\"product_id\":\"" << sym << "\",\"sequence\":" << seq << ",\"time\":\""
      << format_iso_utc(ts_ns) << "\",\"changes\":[";
  bool first = true;
  for (const auto& b : bids) {
    if (!first) oss << ',';
    first = false;
    oss << "[\"buy\",\"" << format_scaled(b.price, pdec) << "\",\"" << format_scaled(b.qty, qdec) << "\"]";
  }
  for (const auto& a : asks) {
    if (!first) oss << ',';
    first = false;
    oss << "[\"sell\",\"" << format_scaled(a.price, pdec) << "\",\"" << format_scaled(a.qty, qdec) << "\"]";
  }
  oss << "]}";
  return oss.str();
}

std::string make_okx_snapshot(const std::string& sym, TimestampNs ts_ns, SeqNum seq, const BookSnapshotData& book,
                              int pdec, int qdec) {
  const int32_t crc = okx_checksum_from_maps(book, pdec, qdec);
  std::ostringstream oss;
  oss << "{\"arg\":{\"channel\":\"books\",\"instId\":\"" << sym << "\"},\"action\":\"snapshot\",\"data\":[{\"asks\":[";
  bool first = true;
  for (const auto& [p, q] : book.asks) {
    if (!first) oss << ',';
    first = false;
    oss << "[\"" << format_scaled(p, pdec) << "\",\"" << format_scaled(q, qdec) << "\",\"0\",\"1\"]";
  }
  oss << "],\"bids\":[";
  first = true;
  for (const auto& [p, q] : book.bids) {
    if (!first) oss << ',';
    first = false;
    oss << "[\"" << format_scaled(p, pdec) << "\",\"" << format_scaled(q, qdec) << "\",\"0\",\"1\"]";
  }
  oss << "],\"ts\":\"" << (ts_ns / kNsPerMs) << "\",\"checksum\":" << crc << ",\"seqId\":" << seq
      << ",\"prevSeqId\":-1}]}";
  return oss.str();
}

std::string make_okx_delta(const std::string& sym, TimestampNs ts_ns, SeqNum seq, SeqNum prev_seq,
                           const std::vector<PriceLevel>& bids, const std::vector<PriceLevel>& asks,
                           const BookSnapshotData& full_book_after, int pdec, int qdec, bool corrupt_crc = false) {
  int32_t crc = okx_checksum_from_maps(full_book_after, pdec, qdec);
  if (corrupt_crc) crc ^= 0x12345678;
  std::ostringstream oss;
  oss << "{\"arg\":{\"channel\":\"books\",\"instId\":\"" << sym << "\"},\"action\":\"update\",\"data\":[{\"asks\":[";
  for (size_t i = 0; i < asks.size(); ++i) {
    if (i > 0) oss << ',';
    oss << "[\"" << format_scaled(asks[i].price, pdec) << "\",\"" << format_scaled(asks[i].qty, qdec)
        << "\",\"0\",\"1\"]";
  }
  oss << "],\"bids\":[";
  for (size_t i = 0; i < bids.size(); ++i) {
    if (i > 0) oss << ',';
    oss << "[\"" << format_scaled(bids[i].price, pdec) << "\",\"" << format_scaled(bids[i].qty, qdec)
        << "\",\"0\",\"1\"]";
  }
  oss << "],\"ts\":\"" << (ts_ns / kNsPerMs) << "\",\"checksum\":" << crc << ",\"seqId\":" << seq
      << ",\"prevSeqId\":" << prev_seq << "}]}";
  return oss.str();
}

BookSnapshotData build_target_book(PriceTicks mid_ticks, size_t levels, PriceTicks half_spread_ticks,
                                   PriceTicks level_step_ticks, QtyLots top_qty, QtyLots deep_qty,
                                   std::mt19937_64& rng) {
  BookSnapshotData b;
  std::uniform_int_distribution<QtyLots> jitter_dist(-deep_qty / 5, deep_qty / 5);
  for (size_t i = 0; i < levels; ++i) {
    const PriceTicks bp = mid_ticks - half_spread_ticks - static_cast<PriceTicks>(i) * level_step_ticks;
    const PriceTicks ap = mid_ticks + half_spread_ticks + static_cast<PriceTicks>(i) * level_step_ticks;
    const QtyLots bq = (i == 0) ? top_qty : std::max<QtyLots>(1000, deep_qty + jitter_dist(rng));
    const QtyLots aq = (i == 0) ? top_qty : std::max<QtyLots>(1000, deep_qty + jitter_dist(rng));
    if (bp > 0) b.bids[bp] = bq;
    if (ap > 0) b.asks[ap] = aq;
  }
  return b;
}

void diff_books(const BookSnapshotData& old_b, const BookSnapshotData& new_b, std::vector<PriceLevel>& bid_deltas,
                std::vector<PriceLevel>& ask_deltas) {
  bid_deltas.clear();
  ask_deltas.clear();
  for (const auto& [p, q] : old_b.bids) {
    auto it = new_b.bids.find(p);
    if (it == new_b.bids.end()) {
      bid_deltas.push_back(PriceLevel{p, 0});
    } else if (it->second != q) {
      bid_deltas.push_back(PriceLevel{p, it->second});
    }
  }
  for (const auto& [p, q] : new_b.bids) {
    if (old_b.bids.find(p) == old_b.bids.end()) {
      bid_deltas.push_back(PriceLevel{p, q});
    }
  }
  for (const auto& [p, q] : old_b.asks) {
    auto it = new_b.asks.find(p);
    if (it == new_b.asks.end()) {
      ask_deltas.push_back(PriceLevel{p, 0});
    } else if (it->second != q) {
      ask_deltas.push_back(PriceLevel{p, it->second});
    }
  }
  for (const auto& [p, q] : new_b.asks) {
    if (old_b.asks.find(p) == old_b.asks.end()) {
      ask_deltas.push_back(PriceLevel{p, q});
    }
  }
}

}  // namespace

std::vector<RecordedFeedEvent> generate_synthetic_feed(const InstrumentRegistry& registry,
                                                       const SynthFeedConfig& cfg) {
  const InstrumentSpec* inst = registry.get(cfg.instrument_id);
  if (!inst) return {};

  const int pdec = inst->price_decimals;
  const int qdec = inst->qty_decimals;
  const std::string& bin_sym = inst->venues[static_cast<size_t>(VenueId::Binance)].native_symbol;
  const std::string& cb_sym = inst->venues[static_cast<size_t>(VenueId::Coinbase)].native_symbol;
  const std::string& okx_sym = inst->venues[static_cast<size_t>(VenueId::Okx)].native_symbol;

  std::mt19937_64 rng(cfg.seed);
  std::uniform_real_distribution<double> u01(0.0, 1.0);
  std::normal_distribution<double> rw_step(0.0, 22.0);  // ~0.22 USD per 5 ms step

  std::vector<RecordedFeedEvent> events;
  events.reserve(cfg.num_steps * 3 + 32);

  std::array<BookSnapshotData, kVenueCount> cur_books;
  std::array<SeqNum, kVenueCount> seqs{1000, 5000, 9000};
  TimestampNs ts = cfg.start_ts_ns;
  PriceTicks fair_mid = cfg.initial_mid_ticks;

  // Initial snapshots for all 3 venues.
  for (size_t v = 0; v < kVenueCount; ++v) {
    cur_books[v] = build_target_book(fair_mid, cfg.book_levels, 15, 25, 180'000, 320'000, rng);
  }

  // Binance authentic startup: emit 1 buffered WS delta, then REST snapshot bridging it!
  {
    BookSnapshotData next_b = build_target_book(fair_mid + 5, cfg.book_levels, 15, 25, 180'000, 320'000, rng);
    std::vector<PriceLevel> bd, ad;
    diff_books(cur_books[0], next_b, bd, ad);
    events.push_back({ts, VenueId::Binance, cfg.instrument_id, FeedEventKind::Reconnect, "init"});
    events.push_back({ts + 100 * kNsPerUs, VenueId::Binance, cfg.instrument_id, FeedEventKind::WsMessage,
                      make_binance_delta(bin_sym, ts + 100 * kNsPerUs, seqs[0] + 1, seqs[0] + 1, bd, ad, pdec, qdec)});
    events.push_back({ts + 500 * kNsPerUs, VenueId::Binance, cfg.instrument_id, FeedEventKind::RestSnapshot,
                      make_binance_snapshot(cur_books[0], seqs[0], pdec, qdec)});
    cur_books[0] = std::move(next_b);
    seqs[0] += 1;
  }

  // Coinbase authentic startup: WS snapshot
  events.push_back({ts + 200 * kNsPerUs, VenueId::Coinbase, cfg.instrument_id, FeedEventKind::WsMessage,
                    make_coinbase_snapshot(cb_sym, ts + 200 * kNsPerUs, seqs[1], cur_books[1], pdec, qdec)});

  // OKX authentic startup: WS snapshot with valid IEEE CRC32
  events.push_back({ts + 300 * kNsPerUs, VenueId::Okx, cfg.instrument_id, FeedEventKind::WsMessage,
                    make_okx_snapshot(okx_sym, ts + 300 * kNsPerUs, seqs[2], cur_books[2], pdec, qdec)});

  // Active cross-venue dislocation state
  int disloc_remaining = 0;
  size_t disloc_venue = 0;
  PriceTicks disloc_offset_ticks = 0;
  bool disloc_thin_depth = false;

  // Fault injection checkpoints
  const size_t binance_gap_step = cfg.inject_faults && cfg.num_steps > 500 ? cfg.num_steps / 4 : 0;
  const size_t coinbase_outage_start = cfg.inject_faults && cfg.num_steps > 500 ? cfg.num_steps / 2 : 0;
  const size_t coinbase_outage_len = 420;  // ~2.1s outage (> 1.5s staleness threshold!)
  const size_t okx_crc_fault_step = cfg.inject_faults && cfg.num_steps > 500 ? (3 * cfg.num_steps) / 4 : 0;

  for (size_t step = 1; step <= cfg.num_steps; ++step) {
    // Advance clock deterministically with mild jitter.
    const TimestampNs dt =
        static_cast<TimestampNs>(static_cast<double>(cfg.step_mean_ns) * (0.7 + 0.6 * u01(rng)));
    ts += dt;

    // Latent fair mid random walk.
    fair_mid = std::max<PriceTicks>(1'000'000, fair_mid + static_cast<PriceTicks>(std::llround(rw_step(rng))));

    // Manage transient dislocations.
    if (disloc_remaining > 0) {
      --disloc_remaining;
      if (disloc_remaining == 0) {
        disloc_offset_ticks = 0;
        disloc_thin_depth = false;
      }
    } else {
      const double r = u01(rng);
      if (r < cfg.fleeting_dislocation_prob) {
        // Fleeting 1-3 step (~5-15 ms) dislocation of 32-55 bps!
        disloc_remaining = 1 + static_cast<int>(rng() % 3ULL);
        disloc_venue = static_cast<size_t>(rng() % kVenueCount);
        const int sign = (rng() % 2ULL == 0) ? 1 : -1;
        const PriceTicks mag = static_cast<PriceTicks>(std::llround(static_cast<double>(fair_mid) * (0.0032 + 0.0023 * u01(rng))));
        disloc_offset_ticks = sign * mag;
        disloc_thin_depth = (u01(rng) < cfg.thin_depth_prob);
      } else if (r < cfg.fleeting_dislocation_prob + cfg.persistent_dislocation_prob) {
        // Persistent 10-22 step (~50-110 ms) dislocation of 30-50 bps!
        disloc_remaining = 10 + static_cast<int>(rng() % 13ULL);
        disloc_venue = static_cast<size_t>(rng() % kVenueCount);
        const int sign = (rng() % 2ULL == 0) ? 1 : -1;
        const PriceTicks mag = static_cast<PriceTicks>(std::llround(static_cast<double>(fair_mid) * (0.0030 + 0.0020 * u01(rng))));
        disloc_offset_ticks = sign * mag;
        disloc_thin_depth = (u01(rng) < cfg.thin_depth_prob);
      } else if (r < cfg.fleeting_dislocation_prob + cfg.persistent_dislocation_prob + cfg.fee_trapped_cross_prob) {
        // Fee-trapped micro-dislocation: 10-16 bps gross spread (< ~20 bps combined taker fees).
        disloc_remaining = 3 + static_cast<int>(rng() % 5ULL);
        disloc_venue = static_cast<size_t>(rng() % kVenueCount);
        const int sign = (rng() % 2ULL == 0) ? 1 : -1;
        const PriceTicks mag = static_cast<PriceTicks>(std::llround(static_cast<double>(fair_mid) * (0.0010 + 0.0006 * u01(rng))));
        disloc_offset_ticks = sign * mag;
        disloc_thin_depth = false;
      }
    }

    for (size_t v = 0; v < kVenueCount; ++v) {
      const TimestampNs v_ts = ts + static_cast<TimestampNs>(v) * 180LL * kNsPerUs;

      // Injected Coinbase silent outage window (tests stale-quote detection & suppression!).
      if (v == static_cast<size_t>(VenueId::Coinbase) && coinbase_outage_start > 0 &&
          step >= coinbase_outage_start && step < coinbase_outage_start + coinbase_outage_len) {
        continue;
      }

      const PriceTicks venue_noise = static_cast<PriceTicks>((static_cast<int>(rng() % 21ULL) - 10));
      const PriceTicks venue_mid = fair_mid + venue_noise + ((disloc_remaining > 0 && v == disloc_venue) ? disloc_offset_ticks : 0);
      const QtyLots top_qty = (disloc_remaining > 0 && v == disloc_venue && disloc_thin_depth) ? 15'000 : 180'000;
      const PriceTicks level_step = (disloc_remaining > 0 && v == disloc_venue && disloc_thin_depth) ? 350 : 30;

      BookSnapshotData next_b = build_target_book(venue_mid, cfg.book_levels, 15, level_step, top_qty, 280'000, rng);
      std::vector<PriceLevel> bd, ad;
      diff_books(cur_books[v], next_b, bd, ad);
      if (bd.empty() && ad.empty()) continue;

      if (v == static_cast<size_t>(VenueId::Binance)) {
        if (binance_gap_step > 0 && step == binance_gap_step) {
          // Inject sequence gap (skip +5), then deliver recovery REST snapshot 45 ms later!
          seqs[0] += 5;
          events.push_back({v_ts, VenueId::Binance, cfg.instrument_id, FeedEventKind::WsMessage,
                            make_binance_delta(bin_sym, v_ts, seqs[0], seqs[0], bd, ad, pdec, qdec)});
          cur_books[0] = std::move(next_b);
          events.push_back({v_ts + 45LL * kNsPerMs, VenueId::Binance, cfg.instrument_id, FeedEventKind::RestSnapshot,
                            make_binance_snapshot(cur_books[0], seqs[0], pdec, qdec)});
        } else {
          seqs[0] += 1;
          events.push_back({v_ts, VenueId::Binance, cfg.instrument_id, FeedEventKind::WsMessage,
                            make_binance_delta(bin_sym, v_ts, seqs[0], seqs[0], bd, ad, pdec, qdec)});
          cur_books[0] = std::move(next_b);
        }
      } else if (v == static_cast<size_t>(VenueId::Coinbase)) {
        // Recover cleanly after outage via disconnect + fresh WS snapshot.
        if (coinbase_outage_start > 0 && step == coinbase_outage_start + coinbase_outage_len) {
          seqs[1] += 50;
          cur_books[1] = std::move(next_b);
          events.push_back({v_ts - 5LL * kNsPerMs, VenueId::Coinbase, cfg.instrument_id, FeedEventKind::Reconnect,
                            "post_outage"});
          events.push_back({v_ts, VenueId::Coinbase, cfg.instrument_id, FeedEventKind::WsMessage,
                            make_coinbase_snapshot(cb_sym, v_ts, seqs[1], cur_books[1], pdec, qdec)});
        } else {
          seqs[1] += 1;
          events.push_back({v_ts, VenueId::Coinbase, cfg.instrument_id, FeedEventKind::WsMessage,
                            make_coinbase_delta(cb_sym, v_ts, seqs[1], bd, ad, pdec, qdec)});
          cur_books[1] = std::move(next_b);
        }
      } else {
        // OKX
        const size_t okx_reconnect_steps = 12;  // ~60ms WS reconnect window after CRC32 fault
        if (okx_crc_fault_step > 0 && step > okx_crc_fault_step && step < okx_crc_fault_step + okx_reconnect_steps) {
          // While OKX WS is reconnecting, no WS deltas arrive; book evolves silently.
          cur_books[2] = std::move(next_b);
          continue;
        }
        const SeqNum prev = seqs[2];
        seqs[2] += 1;
        if (okx_crc_fault_step > 0 && step == okx_crc_fault_step) {
          // Inject CRC32 mismatch; triggers OKX Resyncing + WS reconnect!
          cur_books[2] = std::move(next_b);
          events.push_back({v_ts, VenueId::Okx, cfg.instrument_id, FeedEventKind::WsMessage,
                            make_okx_delta(okx_sym, v_ts, seqs[2], prev, bd, ad, cur_books[2], pdec, qdec,
                                           /*corrupt_crc=*/true)});
        } else if (okx_crc_fault_step > 0 && step == okx_crc_fault_step + okx_reconnect_steps) {
          // Deliver fresh valid OKX WS snapshot after ~60ms reconnect!
          cur_books[2] = std::move(next_b);
          events.push_back({v_ts - 2LL * kNsPerMs, VenueId::Okx, cfg.instrument_id, FeedEventKind::Reconnect,
                            "post_crc_resync"});
          events.push_back({v_ts, VenueId::Okx, cfg.instrument_id, FeedEventKind::WsMessage,
                            make_okx_snapshot(okx_sym, v_ts, seqs[2], cur_books[2], pdec, qdec)});
        } else {
          cur_books[2] = std::move(next_b);
          events.push_back({v_ts, VenueId::Okx, cfg.instrument_id, FeedEventKind::WsMessage,
                            make_okx_delta(okx_sym, v_ts, seqs[2], prev, bd, ad, cur_books[2], pdec, qdec,
                                           /*corrupt_crc=*/false)});
        }
      }
    }
  }

  std::stable_sort(events.begin(), events.end(),
                   [](const RecordedFeedEvent& a, const RecordedFeedEvent& b) { return a.recv_ns < b.recv_ns; });
  return events;
}

}  // namespace xarb

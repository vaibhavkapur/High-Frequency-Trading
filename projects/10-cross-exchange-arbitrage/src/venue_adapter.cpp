#include "xarb/venue_adapter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "xarb/crc32.hpp"
#include "xarb/json_lite.hpp"

namespace xarb {

namespace {

bool parse_iso8601_or_ms_ts(const JsonValue* v, TimestampNs& out_ns) {
  if (!v) return false;
  if (auto ms = v->as_i64()) {
    // Heuristic: values < 1e11 are seconds, < 1e14 are ms, < 1e17 are us, else ns.
    if (*ms < 100'000'000'000LL) out_ns = *ms * kNsPerSec;
    else if (*ms < 100'000'000'000'000LL) out_ns = *ms * kNsPerMs;
    else if (*ms < 100'000'000'000'000'000LL) out_ns = *ms * kNsPerUs;
    else out_ns = *ms;
    return true;
  }
  if (const auto* s = v->as_string()) {
    // Numeric string (OKX uses millisecond string e.g. "1700000000123")
    if (!s->empty() && (*s)[0] >= '0' && (*s)[0] <= '9') {
      auto parsed = parse_decimal_scaled(*s, 0, false);
      if (parsed) {
        if (*parsed < 100'000'000'000LL) out_ns = *parsed * kNsPerSec;
        else if (*parsed < 100'000'000'000'000LL) out_ns = *parsed * kNsPerMs;
        else out_ns = *parsed;
        return true;
      }
    }
    // ISO-8601 UTC: YYYY-MM-DDTHH:MM:SS(.ffffff)Z (used by Coinbase)
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (std::sscanf(s->c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) == 6) {
      // Howard Hinnant civil-to-days algorithm
      int yr = y - (mo <= 2 ? 1 : 0);
      int era = (yr >= 0 ? yr : yr - 399) / 400;
      unsigned yoe = static_cast<unsigned>(yr - era * 400);
      unsigned doy = (153U * static_cast<unsigned>(mo + (mo > 2 ? -3 : 9)) + 2U) / 5U + static_cast<unsigned>(d) - 1U;
      unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
      int64_t days = static_cast<int64_t>(era) * 146097LL + static_cast<int64_t>(doe) - 719468LL;
      int64_t total_sec = days * 86400LL + h * 3600LL + mi * 60LL + sec;
      int64_t frac_ns = 0;
      auto dot = s->find('.');
      if (dot != std::string::npos) {
        int64_t mult = 100'000'000LL;
        for (size_t i = dot + 1; i < s->size() && mult >= 1; ++i) {
          char c = (*s)[i];
          if (c < '0' || c > '9') break;
          frac_ns += static_cast<int64_t>(c - '0') * mult;
          mult /= 10;
        }
      }
      out_ns = total_sec * kNsPerSec + frac_ns;
      return true;
    }
  }
  return false;
}

bool parse_level_array(const JsonValue* arr_val, VenueId venue, const InstrumentSpec& inst,
                       std::vector<PriceLevel>& out) {
  if (!arr_val || !arr_val->is_array()) return false;
  const auto& arr = *arr_val->as_array();
  out.reserve(arr.size());
  for (const auto& entry : arr) {
    const auto* pair = entry.as_array();
    if (!pair || pair->size() < 2) return false;
    const auto* p_str = (*pair)[0].as_string();
    const auto* q_str = (*pair)[1].as_string();
    if (!p_str || !q_str) return false;
    auto p = inst.normalize_price(venue, *p_str);
    auto q = inst.normalize_qty(venue, *q_str);
    if (!p || !q) return false;
    out.push_back(PriceLevel{*p, *q});
  }
  return true;
}

}  // namespace

VenueAdapter::VenueAdapter(VenueId venue, const InstrumentSpec& instrument, TimestampNs max_stale_ns,
                           size_t max_depth, ReconnectPolicy reconnect_policy)
    : venue_(venue),
      instrument_(instrument),
      max_stale_ns_(max_stale_ns),
      reconnect_policy_(reconnect_policy),
      book_(max_depth) {}

TimestampNs VenueAdapter::next_reconnect_backoff_ns() noexcept {
  double factor = std::pow(reconnect_policy_.multiplier, static_cast<double>(consecutive_reconnects_));
  double delay = static_cast<double>(reconnect_policy_.initial_backoff_ns) * factor;
  if (delay > static_cast<double>(reconnect_policy_.max_backoff_ns)) {
    delay = static_cast<double>(reconnect_policy_.max_backoff_ns);
  }
  ++consecutive_reconnects_;
  return static_cast<TimestampNs>(delay);
}

void VenueAdapter::transition_to_synced(TimestampNs now_ns, AdapterAction& act) {
  const bool was_not_synced = (state_ != BookState::Synced);
  if (resync_started_ns_ > 0 && now_ns >= resync_started_ns_) {
    const TimestampNs dur = now_ns - resync_started_ns_;
    metrics_.total_resync_ns += dur;
    metrics_.reconnect_durations_ns.push_back(dur);
    resync_started_ns_ = 0;
  }
  if (stale_started_ns_ > 0 && now_ns >= stale_started_ns_) {
    metrics_.total_stale_ns += (now_ns - stale_started_ns_);
    stale_started_ns_ = 0;
  }
  state_ = BookState::Synced;
  last_sync_ts_ns_ = now_ns;
  consecutive_reconnects_ = 0;
  if (was_not_synced) {
    act.recovered_sync = true;
  }
}

void VenueAdapter::transition_to_resync(TimestampNs now_ns, std::string_view reason, AdapterAction& act) {
  if (state_ != BookState::Resyncing && state_ != BookState::Buffering) {
    resync_started_ns_ = now_ns;
    metrics_.resync_count += 1;
  }
  if (state_ == BookState::Synced) {
    stale_started_ns_ = now_ns;
    metrics_.stale_transitions += 1;
    act.became_stale = true;
  }
  state_ = BookState::Resyncing;
  book_.clear();
  last_seq_ = 0;
  act.reason = std::string(reason);
}

AdapterAction VenueAdapter::on_disconnect(TimestampNs now_ns, std::string_view reason) {
  AdapterAction act;
  metrics_.disconnects += 1;
  if (state_ == BookState::Synced) {
    stale_started_ns_ = now_ns;
    metrics_.stale_transitions += 1;
    act.became_stale = true;
  }
  if (resync_started_ns_ == 0) {
    resync_started_ns_ = now_ns;
  }
  state_ = BookState::Disconnected;
  book_.clear();
  last_seq_ = 0;
  act.reason = std::string(reason);
  return act;
}

AdapterAction VenueAdapter::on_reconnect(TimestampNs now_ns) {
  AdapterAction act;
  if (resync_started_ns_ == 0) {
    resync_started_ns_ = now_ns;
  }
  state_ = BookState::Connecting;
  act.reason = "reconnected";
  return act;
}

AdapterAction VenueAdapter::check_staleness(TimestampNs now_ns) {
  AdapterAction act;
  if (last_recv_ts_ns_ > 0 && now_ns >= last_recv_ts_ns_) {
    const TimestampNs age = now_ns - last_recv_ts_ns_;
    if (metrics_.staleness_samples_ns.size() < 200000) {
      metrics_.staleness_samples_ns.push_back(age);
    }
    if (state_ == BookState::Synced && age > max_stale_ns_) {
      state_ = BookState::Stale;
      stale_started_ns_ = now_ns;
      metrics_.stale_transitions += 1;
      act.became_stale = true;
      act.reason = "quote age exceeded max_stale_ns";
    }
  }
  return act;
}

// ============================================================================
// BinanceAdapter
// ============================================================================

BinanceAdapter::BinanceAdapter(const InstrumentSpec& instrument, TimestampNs max_stale_ns, size_t max_depth,
                               ReconnectPolicy reconnect_policy)
    : VenueAdapter(VenueId::Binance, instrument, max_stale_ns, max_depth, reconnect_policy) {}

bool BinanceAdapter::parse_depth_update(std::string_view payload, TimestampNs recv_ns, BufferedDelta& out,
                                        std::string& err) const {
  auto parsed = parse_json(payload);
  if (!parsed.ok()) {
    err = parsed.error;
    return false;
  }
  const auto& root = *parsed.value;
  if (!root.is_object()) {
    err = "binance depthUpdate must be JSON object";
    return false;
  }
  const auto* ev = root.get("e");
  if (!ev || !ev->as_string() || *ev->as_string() != "depthUpdate") {
    err = "unexpected binance event type";
    return false;
  }
  const auto* sym = root.get("s");
  if (!sym || !sym->as_string() ||
      *sym->as_string() != instrument_.venues[static_cast<size_t>(VenueId::Binance)].native_symbol) {
    err = "symbol mismatch in binance depthUpdate";
    return false;
  }
  const auto* u_first = root.get("U");
  const auto* u_final = root.get("u");
  if (!u_first || !u_final || !u_first->as_u64() || !u_final->as_u64()) {
    err = "missing U/u sequence numbers in binance depthUpdate";
    return false;
  }
  out.first_u = *u_first->as_u64();
  out.final_u = *u_final->as_u64();
  if (out.first_u == 0 || out.final_u < out.first_u) {
    err = "invalid U/u range in binance depthUpdate";
    return false;
  }
  TimestampNs exch_ts = recv_ns;
  parse_iso8601_or_ms_ts(root.get("E"), exch_ts);
  out.exch_ts_ns = exch_ts;
  out.recv_ts_ns = recv_ns;
  if (!parse_level_array(root.get("b"), VenueId::Binance, instrument_, out.bids) ||
      !parse_level_array(root.get("a"), VenueId::Binance, instrument_, out.asks)) {
    err = "malformed bid/ask array in binance depthUpdate";
    return false;
  }
  return true;
}

void BinanceAdapter::apply_parsed_delta(const BufferedDelta& d) {
  for (const auto& b : d.bids) book_.apply_delta(Side::Bid, b.price, b.qty);
  for (const auto& a : d.asks) book_.apply_delta(Side::Ask, a.price, a.qty);
  last_seq_ = d.final_u;
  last_exchange_ts_ns_ = d.exch_ts_ns;
  last_recv_ts_ns_ = d.recv_ts_ns;
  metrics_.deltas_applied += 1;
}

AdapterAction BinanceAdapter::on_disconnect(TimestampNs now_ns, std::string_view reason) {
  buffer_.clear();
  snapshot_requested_ = false;
  return VenueAdapter::on_disconnect(now_ns, reason);
}

AdapterAction BinanceAdapter::on_reconnect(TimestampNs now_ns) {
  auto act = VenueAdapter::on_reconnect(now_ns);
  buffer_.clear();
  state_ = BookState::Buffering;
  snapshot_requested_ = true;
  act.needs_rest_snapshot = true;
  return act;
}

AdapterAction BinanceAdapter::on_ws_message(std::string_view payload, TimestampNs recv_ns) {
  AdapterAction act;
  metrics_.messages_received += 1;

  BufferedDelta delta;
  std::string err;
  if (!parse_depth_update(payload, recv_ns, delta, err)) {
    metrics_.parse_errors += 1;
    act.parse_error = true;
    act.reason = err;
    return act;
  }

  // If disconnected/connecting/buffering/resyncing, buffer deltas and ensure REST snapshot is requested.
  if (state_ != BookState::Synced && state_ != BookState::Stale) {
    if (state_ == BookState::Disconnected || state_ == BookState::Connecting) {
      state_ = BookState::Buffering;
      if (resync_started_ns_ == 0) resync_started_ns_ = recv_ns;
    }
    if (buffer_.size() >= kMaxBufferedDeltas) {
      buffer_.pop_front();
    }
    buffer_.push_back(std::move(delta));
    if (!snapshot_requested_) {
      snapshot_requested_ = true;
      act.needs_rest_snapshot = true;
      act.reason = "buffering deltas awaiting REST snapshot";
    }
    return act;
  }

  // Currently Synced or Stale: enforce strict Binance delta continuity.
  if (delta.final_u <= last_seq_) {
    metrics_.duplicates_ignored += 1;
    return act;
  }

  if (delta.first_u != last_seq_ + 1) {
    // Sequence gap detected!
    metrics_.gaps_detected += 1;
    act.gap_detected = true;
    transition_to_resync(recv_ns, "binance sequence gap: U != prev_u + 1", act);
    buffer_.clear();
    buffer_.push_back(std::move(delta));
    snapshot_requested_ = true;
    act.needs_rest_snapshot = true;
    return act;
  }

  apply_parsed_delta(delta);
  if (state_ == BookState::Stale) {
    transition_to_synced(recv_ns, act);
  }
  act.book_updated = true;
  return act;
}

AdapterAction BinanceAdapter::on_rest_snapshot(std::string_view payload, TimestampNs recv_ns) {
  AdapterAction act;
  snapshot_requested_ = false;

  auto parsed = parse_json(payload);
  if (!parsed.ok() || !parsed.value->is_object()) {
    metrics_.parse_errors += 1;
    act.parse_error = true;
    act.reason = parsed.ok() ? "binance snapshot must be JSON object" : parsed.error;
    return act;
  }
  const auto& root = *parsed.value;
  const auto* last_id_v = root.get("lastUpdateId");
  if (!last_id_v || !last_id_v->as_u64()) {
    metrics_.parse_errors += 1;
    act.parse_error = true;
    act.reason = "missing lastUpdateId in binance snapshot";
    return act;
  }
  const SeqNum snap_id = *last_id_v->as_u64();

  std::vector<PriceLevel> bids;
  std::vector<PriceLevel> asks;
  if (!parse_level_array(root.get("bids"), VenueId::Binance, instrument_, bids) ||
      !parse_level_array(root.get("asks"), VenueId::Binance, instrument_, asks)) {
    metrics_.parse_errors += 1;
    act.parse_error = true;
    act.reason = "malformed bids/asks in binance snapshot";
    return act;
  }

  // Drop buffered deltas with u <= snap_id.
  while (!buffer_.empty() && buffer_.front().final_u <= snap_id) {
    buffer_.pop_front();
  }

  // If we already buffered newer deltas, the first remaining delta must bridge snap_id + 1.
  if (!buffer_.empty()) {
    const auto& first = buffer_.front();
    if (first.first_u > snap_id + 1) {
      // Snapshot is already older than our earliest buffered delta -> must re-fetch snapshot!
      metrics_.gaps_detected += 1;
      act.gap_detected = true;
      transition_to_resync(recv_ns, "binance snapshot stale relative to buffered WS deltas", act);
      snapshot_requested_ = true;
      act.needs_rest_snapshot = true;
      return act;
    }
  }

  book_.apply_snapshot(bids, asks);
  last_seq_ = snap_id;
  last_recv_ts_ns_ = recv_ns;
  last_exchange_ts_ns_ = recv_ns;
  metrics_.snapshots_applied += 1;

  // Replay buffered deltas adhering strictly to Binance continuity.
  bool first_applied = false;
  while (!buffer_.empty()) {
    auto d = std::move(buffer_.front());
    buffer_.pop_front();
    if (d.final_u <= last_seq_) {
      metrics_.duplicates_ignored += 1;
      continue;
    }
    if (!first_applied) {
      if (!(d.first_u <= last_seq_ + 1 && d.final_u >= last_seq_ + 1)) {
        metrics_.gaps_detected += 1;
        act.gap_detected = true;
        transition_to_resync(recv_ns, "binance bridge invariant violated", act);
        snapshot_requested_ = true;
        act.needs_rest_snapshot = true;
        return act;
      }
      first_applied = true;
    } else if (d.first_u != last_seq_ + 1) {
      metrics_.gaps_detected += 1;
      act.gap_detected = true;
      transition_to_resync(recv_ns, "binance buffered delta discontinuity", act);
      buffer_.clear();
      snapshot_requested_ = true;
      act.needs_rest_snapshot = true;
      return act;
    }
    apply_parsed_delta(d);
  }

  transition_to_synced(recv_ns, act);
  act.book_updated = true;
  return act;
}

// ============================================================================
// CoinbaseAdapter
// ============================================================================

CoinbaseAdapter::CoinbaseAdapter(const InstrumentSpec& instrument, TimestampNs max_stale_ns, size_t max_depth,
                                 ReconnectPolicy reconnect_policy)
    : VenueAdapter(VenueId::Coinbase, instrument, max_stale_ns, max_depth, reconnect_policy) {}

AdapterAction CoinbaseAdapter::on_rest_snapshot(std::string_view /*payload*/, TimestampNs /*recv_ns*/) {
  // Coinbase L2 sync uses the WS initial snapshot rather than REST.
  AdapterAction act;
  act.reason = "coinbase does not use REST snapshot recovery";
  return act;
}

AdapterAction CoinbaseAdapter::on_ws_message(std::string_view payload, TimestampNs recv_ns) {
  AdapterAction act;
  metrics_.messages_received += 1;

  auto parsed = parse_json(payload);
  if (!parsed.ok() || !parsed.value->is_object()) {
    metrics_.parse_errors += 1;
    act.parse_error = true;
    act.reason = parsed.ok() ? "coinbase message must be JSON object" : parsed.error;
    return act;
  }
  const auto& root = *parsed.value;
  const auto* type_v = root.get("type");
  const auto* prod_v = root.get("product_id");
  if (!type_v || !type_v->as_string() || !prod_v || !prod_v->as_string()) {
    metrics_.parse_errors += 1;
    act.parse_error = true;
    act.reason = "missing type or product_id in coinbase message";
    return act;
  }
  if (*prod_v->as_string() != instrument_.venues[static_cast<size_t>(VenueId::Coinbase)].native_symbol) {
    metrics_.parse_errors += 1;
    act.parse_error = true;
    act.reason = "product_id mismatch in coinbase message";
    return act;
  }

  const auto* seq_v = root.get("sequence");
  if (!seq_v || !seq_v->as_u64()) {
    metrics_.parse_errors += 1;
    act.parse_error = true;
    act.reason = "missing sequence in coinbase message";
    return act;
  }
  const SeqNum seq = *seq_v->as_u64();

  TimestampNs exch_ts = recv_ns;
  parse_iso8601_or_ms_ts(root.get("time"), exch_ts);

  const std::string& type = *type_v->as_string();
  if (type == "snapshot") {
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    if (!parse_level_array(root.get("bids"), VenueId::Coinbase, instrument_, bids) ||
        !parse_level_array(root.get("asks"), VenueId::Coinbase, instrument_, asks)) {
      metrics_.parse_errors += 1;
      act.parse_error = true;
      act.reason = "malformed bids/asks in coinbase snapshot";
      return act;
    }
    book_.apply_snapshot(bids, asks);
    last_seq_ = seq;
    last_exchange_ts_ns_ = exch_ts;
    last_recv_ts_ns_ = recv_ns;
    metrics_.snapshots_applied += 1;
    transition_to_synced(recv_ns, act);
    act.book_updated = true;
    return act;
  }

  if (state_ != BookState::Synced && state_ != BookState::Stale) {
    // Delta or heartbeat arrived before WS snapshot.
    act.needs_ws_reconnect = true;
    act.reason = "coinbase message received before snapshot";
    return act;
  }

  if (seq <= last_seq_) {
    metrics_.duplicates_ignored += 1;
    return act;
  }

  if (seq != last_seq_ + 1) {
    metrics_.gaps_detected += 1;
    act.gap_detected = true;
    transition_to_resync(recv_ns, "coinbase sequence gap: seq != last_seq + 1", act);
    act.needs_ws_reconnect = true;
    return act;
  }

  if (type == "heartbeat") {
    last_seq_ = seq;
    last_exchange_ts_ns_ = exch_ts;
    last_recv_ts_ns_ = recv_ns;
    metrics_.heartbeats_received += 1;
    if (state_ == BookState::Stale) {
      transition_to_synced(recv_ns, act);
    }
    return act;
  }

  if (type == "l2update") {
    const auto* changes_v = root.get("changes");
    if (!changes_v || !changes_v->is_array()) {
      metrics_.parse_errors += 1;
      act.parse_error = true;
      act.reason = "missing changes array in coinbase l2update";
      return act;
    }
    for (const auto& entry : *changes_v->as_array()) {
      const auto* triple = entry.as_array();
      if (!triple || triple->size() < 3) {
        metrics_.parse_errors += 1;
        act.parse_error = true;
        act.reason = "malformed change tuple in coinbase l2update";
        return act;
      }
      const auto* side_s = (*triple)[0].as_string();
      const auto* p_s = (*triple)[1].as_string();
      const auto* q_s = (*triple)[2].as_string();
      if (!side_s || !p_s || !q_s) {
        metrics_.parse_errors += 1;
        act.parse_error = true;
        act.reason = "non-string element in coinbase change tuple";
        return act;
      }
      Side side = Side::Bid;
      if (*side_s == "buy") side = Side::Bid;
      else if (*side_s == "sell") side = Side::Ask;
      else {
        metrics_.parse_errors += 1;
        act.parse_error = true;
        act.reason = "invalid side in coinbase change tuple";
        return act;
      }
      auto p = instrument_.normalize_price(VenueId::Coinbase, *p_s);
      auto q = instrument_.normalize_qty(VenueId::Coinbase, *q_s);
      if (!p || !q) {
        metrics_.parse_errors += 1;
        act.parse_error = true;
        act.reason = "invalid price/qty in coinbase l2update";
        return act;
      }
      book_.apply_delta(side, *p, *q);
    }
    last_seq_ = seq;
    last_exchange_ts_ns_ = exch_ts;
    last_recv_ts_ns_ = recv_ns;
    metrics_.deltas_applied += 1;
    if (state_ == BookState::Stale) {
      transition_to_synced(recv_ns, act);
    }
    act.book_updated = true;
    return act;
  }

  metrics_.parse_errors += 1;
  act.parse_error = true;
  act.reason = "unknown coinbase message type";
  return act;
}

// ============================================================================
// OkxAdapter
// ============================================================================

OkxAdapter::OkxAdapter(const InstrumentSpec& instrument, TimestampNs max_stale_ns, size_t max_depth,
                       ReconnectPolicy reconnect_policy)
    : VenueAdapter(VenueId::Okx, instrument, max_stale_ns, max_depth, reconnect_policy) {}

AdapterAction OkxAdapter::on_rest_snapshot(std::string_view /*payload*/, TimestampNs /*recv_ns*/) {
  AdapterAction act;
  act.reason = "okx uses WS books snapshot recovery";
  return act;
}

AdapterAction OkxAdapter::on_disconnect(TimestampNs now_ns, std::string_view reason) {
  clear_raw_levels();
  return VenueAdapter::on_disconnect(now_ns, reason);
}

void OkxAdapter::clear_raw_levels() noexcept {
  raw_bids_.clear();
  raw_asks_.clear();
}

void OkxAdapter::set_raw_level(Side side, PriceTicks price, QtyLots qty, std::string price_str, std::string qty_str) {
  if (side == Side::Bid) {
    if (qty <= 0) raw_bids_.erase(price);
    else raw_bids_[price] = RawLevelStr{std::move(price_str), std::move(qty_str)};
  } else {
    if (qty <= 0) raw_asks_.erase(price);
    else raw_asks_[price] = RawLevelStr{std::move(price_str), std::move(qty_str)};
  }
}

int32_t OkxAdapter::compute_top25_checksum() const {
  std::string payload;
  payload.reserve(1024);
  auto bit = raw_bids_.begin();
  auto ait = raw_asks_.begin();
  for (int i = 0; i < 25; ++i) {
    const bool has_b = (bit != raw_bids_.end());
    const bool has_a = (ait != raw_asks_.end());
    if (!has_b && !has_a) break;
    if (has_b) {
      if (!payload.empty()) payload.push_back(':');
      payload += bit->second.price_str;
      payload.push_back(':');
      payload += bit->second.qty_str;
      ++bit;
    }
    if (has_a) {
      if (!payload.empty()) payload.push_back(':');
      payload += ait->second.price_str;
      payload.push_back(':');
      payload += ait->second.qty_str;
      ++ait;
    }
  }
  return Crc32::compute_signed(payload);
}

AdapterAction OkxAdapter::on_ws_message(std::string_view payload, TimestampNs recv_ns) {
  AdapterAction act;
  metrics_.messages_received += 1;

  auto parsed = parse_json(payload);
  if (!parsed.ok() || !parsed.value->is_object()) {
    metrics_.parse_errors += 1;
    act.parse_error = true;
    act.reason = parsed.ok() ? "okx message must be JSON object" : parsed.error;
    return act;
  }
  const auto& root = *parsed.value;
  const auto* arg_v = root.get("arg");
  const auto* action_v = root.get("action");
  const auto* data_v = root.get("data");
  if (!arg_v || !action_v || !action_v->as_string() || !data_v || !data_v->as_array() || data_v->as_array()->empty()) {
    metrics_.parse_errors += 1;
    act.parse_error = true;
    act.reason = "missing arg/action/data in okx message";
    return act;
  }

  const auto* inst_v = arg_v->get("instId");
  if (!inst_v || !inst_v->as_string() ||
      *inst_v->as_string() != instrument_.venues[static_cast<size_t>(VenueId::Okx)].native_symbol) {
    metrics_.parse_errors += 1;
    act.parse_error = true;
    act.reason = "instId mismatch in okx message";
    return act;
  }

  const auto& item = (*data_v->as_array())[0];
  if (!item.is_object()) {
    metrics_.parse_errors += 1;
    act.parse_error = true;
    act.reason = "okx data[0] must be object";
    return act;
  }

  const auto* seq_v = item.get("seqId");
  const auto* prev_seq_v = item.get("prevSeqId");
  const auto* checksum_v = item.get("checksum");
  if (!seq_v || !seq_v->as_i64() || !prev_seq_v || !prev_seq_v->as_i64()) {
    metrics_.parse_errors += 1;
    act.parse_error = true;
    act.reason = "missing seqId/prevSeqId in okx message";
    return act;
  }

  const int64_t seq_i64 = *seq_v->as_i64();
  const int64_t prev_seq_i64 = *prev_seq_v->as_i64();
  if (seq_i64 < 0) {
    metrics_.parse_errors += 1;
    act.parse_error = true;
    act.reason = "negative seqId in okx message";
    return act;
  }
  const SeqNum seq = static_cast<SeqNum>(seq_i64);

  TimestampNs exch_ts = recv_ns;
  parse_iso8601_or_ms_ts(item.get("ts"), exch_ts);

  const std::string& action = *action_v->as_string();
  const auto* bids_arr = item.get("bids");
  const auto* asks_arr = item.get("asks");
  if (!bids_arr || !bids_arr->is_array() || !asks_arr || !asks_arr->is_array()) {
    metrics_.parse_errors += 1;
    act.parse_error = true;
    act.reason = "missing bids/asks in okx data";
    return act;
  }

  auto parse_raw_levels = [&](const JsonValue::Array& arr, Side side,
                              std::vector<std::tuple<PriceTicks, QtyLots, std::string, std::string>>& out) -> bool {
    out.reserve(arr.size());
    for (const auto& entry : arr) {
      const auto* tuple = entry.as_array();
      if (!tuple || tuple->size() < 2) return false;
      const auto* p_s = (*tuple)[0].as_string();
      const auto* q_s = (*tuple)[1].as_string();
      if (!p_s || !q_s) return false;
      auto p = instrument_.normalize_price(VenueId::Okx, *p_s);
      auto q = instrument_.normalize_qty(VenueId::Okx, *q_s);
      if (!p || !q) return false;
      (void)side;
      out.emplace_back(*p, *q, *p_s, *q_s);
    }
    return true;
  };

  if (action == "snapshot") {
    std::vector<std::tuple<PriceTicks, QtyLots, std::string, std::string>> b_list;
    std::vector<std::tuple<PriceTicks, QtyLots, std::string, std::string>> a_list;
    if (!parse_raw_levels(*bids_arr->as_array(), Side::Bid, b_list) ||
        !parse_raw_levels(*asks_arr->as_array(), Side::Ask, a_list)) {
      metrics_.parse_errors += 1;
      act.parse_error = true;
      act.reason = "malformed levels in okx snapshot";
      return act;
    }

    clear_raw_levels();
    std::vector<PriceLevel> norm_bids;
    std::vector<PriceLevel> norm_asks;
    norm_bids.reserve(b_list.size());
    norm_asks.reserve(a_list.size());
    for (auto& [p, q, ps, qs] : b_list) {
      if (q > 0) {
        norm_bids.push_back(PriceLevel{p, q});
        set_raw_level(Side::Bid, p, q, std::move(ps), std::move(qs));
      }
    }
    for (auto& [p, q, ps, qs] : a_list) {
      if (q > 0) {
        norm_asks.push_back(PriceLevel{p, q});
        set_raw_level(Side::Ask, p, q, std::move(ps), std::move(qs));
      }
    }
    book_.apply_snapshot(norm_bids, norm_asks);

    if (checksum_v && checksum_v->as_i64()) {
      const int32_t expected = static_cast<int32_t>(*checksum_v->as_i64());
      const int32_t actual = compute_top25_checksum();
      if (expected != actual) {
        metrics_.checksum_mismatches += 1;
        act.checksum_mismatch = true;
        clear_raw_levels();
        transition_to_resync(recv_ns, "okx snapshot CRC32 checksum mismatch", act);
        act.needs_ws_reconnect = true;
        return act;
      }
    }

    last_seq_ = seq;
    last_exchange_ts_ns_ = exch_ts;
    last_recv_ts_ns_ = recv_ns;
    metrics_.snapshots_applied += 1;
    transition_to_synced(recv_ns, act);
    act.book_updated = true;
    return act;
  }

  if (action != "update") {
    metrics_.parse_errors += 1;
    act.parse_error = true;
    act.reason = "unknown okx action";
    return act;
  }

  if (state_ != BookState::Synced && state_ != BookState::Stale) {
    act.needs_ws_reconnect = true;
    act.reason = "okx update received before snapshot";
    return act;
  }

  // OKX no-op heartbeat: prevSeqId == seqId and empty bids/asks
  if (prev_seq_i64 == seq_i64 && bids_arr->as_array()->empty() && asks_arr->as_array()->empty()) {
    last_exchange_ts_ns_ = exch_ts;
    last_recv_ts_ns_ = recv_ns;
    metrics_.heartbeats_received += 1;
    if (state_ == BookState::Stale) {
      transition_to_synced(recv_ns, act);
    }
    return act;
  }

  if (seq <= last_seq_) {
    metrics_.duplicates_ignored += 1;
    return act;
  }

  if (prev_seq_i64 < 0 || static_cast<SeqNum>(prev_seq_i64) != last_seq_) {
    metrics_.gaps_detected += 1;
    act.gap_detected = true;
    clear_raw_levels();
    transition_to_resync(recv_ns, "okx sequence gap: prevSeqId != last_seq", act);
    act.needs_ws_reconnect = true;
    return act;
  }

  std::vector<std::tuple<PriceTicks, QtyLots, std::string, std::string>> b_list;
  std::vector<std::tuple<PriceTicks, QtyLots, std::string, std::string>> a_list;
  if (!parse_raw_levels(*bids_arr->as_array(), Side::Bid, b_list) ||
      !parse_raw_levels(*asks_arr->as_array(), Side::Ask, a_list)) {
    metrics_.parse_errors += 1;
    act.parse_error = true;
    act.reason = "malformed levels in okx update";
    return act;
  }

  for (auto& [p, q, ps, qs] : b_list) {
    book_.apply_delta(Side::Bid, p, q);
    set_raw_level(Side::Bid, p, q, std::move(ps), std::move(qs));
  }
  for (auto& [p, q, ps, qs] : a_list) {
    book_.apply_delta(Side::Ask, p, q);
    set_raw_level(Side::Ask, p, q, std::move(ps), std::move(qs));
  }

  if (checksum_v && checksum_v->as_i64()) {
    const int32_t expected = static_cast<int32_t>(*checksum_v->as_i64());
    const int32_t actual = compute_top25_checksum();
    if (expected != actual) {
      metrics_.checksum_mismatches += 1;
      act.checksum_mismatch = true;
      clear_raw_levels();
      transition_to_resync(recv_ns, "okx update CRC32 checksum mismatch", act);
      act.needs_ws_reconnect = true;
      return act;
    }
  }

  last_seq_ = seq;
  last_exchange_ts_ns_ = exch_ts;
  last_recv_ts_ns_ = recv_ns;
  metrics_.deltas_applied += 1;
  if (state_ == BookState::Stale) {
    transition_to_synced(recv_ns, act);
  }
  act.book_updated = true;
  return act;
}

std::unique_ptr<VenueAdapter> make_venue_adapter(VenueId venue, const InstrumentSpec& instrument,
                                                 TimestampNs max_stale_ns, size_t max_depth,
                                                 ReconnectPolicy reconnect_policy) {
  switch (venue) {
    case VenueId::Binance:
      return std::make_unique<BinanceAdapter>(instrument, max_stale_ns, max_depth, reconnect_policy);
    case VenueId::Coinbase:
      return std::make_unique<CoinbaseAdapter>(instrument, max_stale_ns, max_depth, reconnect_policy);
    case VenueId::Okx:
      return std::make_unique<OkxAdapter>(instrument, max_stale_ns, max_depth, reconnect_policy);
    case VenueId::Count:
      break;
  }
  return nullptr;
}

}  // namespace xarb

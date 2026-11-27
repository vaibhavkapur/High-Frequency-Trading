#include "mmae/feature_engine.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mmae {

const char* to_string(Side side) {
    switch (side) {
        case Side::BID: return "BID";
        case Side::ASK: return "ASK";
        case Side::UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

const char* to_string(EventType type) {
    switch (type) {
        case EventType::L2_SNAPSHOT_CLEAR: return "L2_SNAPSHOT_CLEAR";
        case EventType::L2_SET: return "L2_SET";
        case EventType::L3_ADD: return "L3_ADD";
        case EventType::L3_MODIFY: return "L3_MODIFY";
        case EventType::L3_CANCEL: return "L3_CANCEL";
        case EventType::L3_EXECUTE: return "L3_EXECUTE";
        case EventType::TRADE: return "TRADE";
        case EventType::HEARTBEAT: return "HEARTBEAT";
    }
    return "UNKNOWN";
}

const char* to_string(TradeSign sign) {
    switch (sign) {
        case TradeSign::BUY: return "BUY";
        case TradeSign::SELL: return "SELL";
        case TradeSign::AMBIGUOUS: return "AMBIGUOUS";
        case TradeSign::NONE: return "NONE";
    }
    return "NONE";
}

const char* to_string(ClassificationReason reason) {
    switch (reason) {
        case ClassificationReason::NOT_A_TRADE: return "NOT_A_TRADE";
        case ClassificationReason::QUOTE_AT_OR_ABOVE_ASK: return "QUOTE_AT_OR_ABOVE_ASK";
        case ClassificationReason::QUOTE_AT_OR_BELOW_BID: return "QUOTE_AT_OR_BELOW_BID";
        case ClassificationReason::QUOTE_ABOVE_MID: return "QUOTE_ABOVE_MID";
        case ClassificationReason::QUOTE_BELOW_MID: return "QUOTE_BELOW_MID";
        case ClassificationReason::MIDPOINT_AMBIGUOUS: return "MIDPOINT_AMBIGUOUS";
        case ClassificationReason::BOOK_INCOMPLETE: return "BOOK_INCOMPLETE";
        case ClassificationReason::BOOK_CROSSED: return "BOOK_CROSSED";
        case ClassificationReason::BOOK_LOCKED: return "BOOK_LOCKED";
        case ClassificationReason::BOOK_STALE: return "BOOK_STALE";
        case ClassificationReason::GAP_ACTIVE: return "GAP_ACTIVE";
    }
    return "NOT_A_TRADE";
}

const char* to_string(ValidationErrorCode code) {
    switch (code) {
        case ValidationErrorCode::NONE: return "NONE";
        case ValidationErrorCode::MALFORMED_ROW: return "MALFORMED_ROW";
        case ValidationErrorCode::UNKNOWN_EVENT_TYPE: return "UNKNOWN_EVENT_TYPE";
        case ValidationErrorCode::INVALID_SIDE: return "INVALID_SIDE";
        case ValidationErrorCode::INVALID_TIMESTAMP: return "INVALID_TIMESTAMP";
        case ValidationErrorCode::OUT_OF_ORDER_TS: return "OUT_OF_ORDER_TS";
        case ValidationErrorCode::DUPLICATE_OR_REGRESSING_SEQ: return "DUPLICATE_OR_REGRESSING_SEQ";
        case ValidationErrorCode::SYMBOL_MISMATCH: return "SYMBOL_MISMATCH";
        case ValidationErrorCode::INVALID_PRICE: return "INVALID_PRICE";
        case ValidationErrorCode::INVALID_QTY: return "INVALID_QTY";
        case ValidationErrorCode::INVALID_ORDER_ID: return "INVALID_ORDER_ID";
        case ValidationErrorCode::BOOK_CAPACITY_LEVELS_EXCEEDED: return "BOOK_CAPACITY_LEVELS_EXCEEDED";
        case ValidationErrorCode::BOOK_CAPACITY_ORDERS_EXCEEDED: return "BOOK_CAPACITY_ORDERS_EXCEEDED";
        case ValidationErrorCode::BOOK_DUPLICATE_ORDER_ID: return "BOOK_DUPLICATE_ORDER_ID";
        case ValidationErrorCode::BOOK_ORDER_NOT_FOUND: return "BOOK_ORDER_NOT_FOUND";
    }
    return "NONE";
}

bool parse_side(const std::string& s, Side& out) {
    if (s == "BID" || s == "B") {
        out = Side::BID;
        return true;
    }
    if (s == "ASK" || s == "A") {
        out = Side::ASK;
        return true;
    }
    if (s == "UNKNOWN" || s == "U" || s.empty()) {
        out = Side::UNKNOWN;
        return true;
    }
    return false;
}

bool parse_event_type(const std::string& s, EventType& out) {
    if (s == "L2_SNAPSHOT_CLEAR") { out = EventType::L2_SNAPSHOT_CLEAR; return true; }
    if (s == "L2_SET") { out = EventType::L2_SET; return true; }
    if (s == "L3_ADD") { out = EventType::L3_ADD; return true; }
    if (s == "L3_MODIFY") { out = EventType::L3_MODIFY; return true; }
    if (s == "L3_CANCEL") { out = EventType::L3_CANCEL; return true; }
    if (s == "L3_EXECUTE") { out = EventType::L3_EXECUTE; return true; }
    if (s == "TRADE") { out = EventType::TRADE; return true; }
    if (s == "HEARTBEAT") { out = EventType::HEARTBEAT; return true; }
    return false;
}

bool validate_config(const EngineConfig& cfg, std::string& error_out) {
    if (!std::isfinite(cfg.tick_size) || cfg.tick_size <= 0.0) {
        error_out = "tick_size must be finite and > 0";
        return false;
    }
    if (cfg.stale_threshold_ns <= 0) {
        error_out = "stale_threshold_ns must be > 0";
        return false;
    }
    if (cfg.flow_window_ns <= 0) {
        error_out = "flow_window_ns must be > 0";
        return false;
    }
    if (cfg.depth_levels_k == 0 || cfg.depth_levels_k > cfg.max_levels) {
        error_out = "depth_levels_k must be in [1, max_levels]";
        return false;
    }
    if (cfg.max_levels == 0 || cfg.max_orders == 0) {
        error_out = "max_levels and max_orders must be > 0";
        return false;
    }
    if (cfg.variant != "baseline" && cfg.variant != "optimized") {
        error_out = "variant must be 'baseline' or 'optimized'";
        return false;
    }
    return true;
}

namespace {

ValidationErrorCode map_book_error(BookErrorCode ec) {
    switch (ec) {
        case BookErrorCode::OK: return ValidationErrorCode::NONE;
        case BookErrorCode::CAPACITY_LEVELS_EXCEEDED: return ValidationErrorCode::BOOK_CAPACITY_LEVELS_EXCEEDED;
        case BookErrorCode::CAPACITY_ORDERS_EXCEEDED: return ValidationErrorCode::BOOK_CAPACITY_ORDERS_EXCEEDED;
        case BookErrorCode::DUPLICATE_ORDER_ID: return ValidationErrorCode::BOOK_DUPLICATE_ORDER_ID;
        case BookErrorCode::ORDER_NOT_FOUND: return ValidationErrorCode::BOOK_ORDER_NOT_FOUND;
        case BookErrorCode::INVALID_PRICE: return ValidationErrorCode::INVALID_PRICE;
        case BookErrorCode::INVALID_QTY: return ValidationErrorCode::INVALID_QTY;
        case BookErrorCode::INVALID_SIDE: return ValidationErrorCode::INVALID_SIDE;
    }
    return ValidationErrorCode::MALFORMED_ROW;
}

}  // namespace

FeatureEngine::FeatureEngine(const EngineConfig& config)
    : config_(config) {
    std::string cfg_err;
    if (!validate_config(config_, cfg_err)) {
        throw std::invalid_argument("Invalid EngineConfig: " + cfg_err);
    }
    book_ = make_order_book(config_);

    diagnostics_.schema_version = kSchemaVersion;
    diagnostics_.engine_variant = book_->variant_name();
    diagnostics_.tick_size = config_.tick_size;
    diagnostics_.stale_threshold_ns = config_.stale_threshold_ns;
    diagnostics_.flow_window_ns = config_.flow_window_ns;
    diagnostics_.depth_levels_k = config_.depth_levels_k;
    diagnostics_.max_levels = config_.max_levels;
    diagnostics_.max_orders = config_.max_orders;
}

void FeatureEngine::record_error(
    size_t line_number, uint64_t seq_num, ValidationErrorCode code, const std::string& detail
) {
    ++diagnostics_.rejected_events;
    if (diagnostics_.errors.size() < config_.max_recorded_errors) {
        diagnostics_.errors.push_back(ValidationErrorRecord{line_number, seq_num, code, detail});
    }
}

void FeatureEngine::prune_windows(int64_t now_ts_ns) {
    int64_t cutoff = now_ts_ns - config_.flow_window_ns;
    while (!ofi_window_.empty() && ofi_window_.front().ts_ns < cutoff) {
        rolling_ofi_sum_ -= ofi_window_.front().ofi;
        ofi_window_.pop_front();
    }
    while (!trade_window_.empty() && trade_window_.front().ts_ns < cutoff) {
        rolling_signed_flow_qty_ -= trade_window_.front().signed_qty;
        rolling_total_trade_qty_ -= trade_window_.front().total_qty;
        rolling_ambiguous_qty_ -= trade_window_.front().ambiguous_qty;
        trade_window_.pop_front();
    }
}

double FeatureEngine::compute_event_ofi(const TopOfBook& prev, const TopOfBook& curr) const {
    if (!prev.has_bid || !prev.has_ask || !curr.has_bid || !curr.has_ask) {
        return 0.0;
    }
    double delta_bid = 0.0;
    if (curr.best_bid_ticks > prev.best_bid_ticks) {
        delta_bid = static_cast<double>(curr.bid_qty_l1);
    } else if (curr.best_bid_ticks == prev.best_bid_ticks) {
        delta_bid = static_cast<double>(curr.bid_qty_l1 - prev.bid_qty_l1);
    } else {
        delta_bid = -static_cast<double>(prev.bid_qty_l1);
    }

    double delta_ask = 0.0;
    if (curr.best_ask_ticks < prev.best_ask_ticks) {
        delta_ask = static_cast<double>(curr.ask_qty_l1);
    } else if (curr.best_ask_ticks == prev.best_ask_ticks) {
        delta_ask = static_cast<double>(curr.ask_qty_l1 - prev.ask_qty_l1);
    } else {
        delta_ask = -static_cast<double>(prev.ask_qty_l1);
    }
    return delta_bid - delta_ask;
}

ProcessResult FeatureEngine::process_event(const Event& event, size_t line_number) {
    ProcessResult res{};
    ++diagnostics_.total_input_rows;

    // 1. Validate canonical schema constraints before mutating any state.
    if (event.event_ts_ns <= 0 || event.recv_ts_ns < 0) {
        res.error = ValidationErrorCode::INVALID_TIMESTAMP;
        res.error_detail = "event_ts_ns must be > 0 and recv_ts_ns >= 0";
        record_error(line_number, event.seq_num, res.error, res.error_detail);
        return res;
    }
    if (event.seq_num == 0) {
        res.error = ValidationErrorCode::DUPLICATE_OR_REGRESSING_SEQ;
        res.error_detail = "seq_num must be >= 1";
        ++diagnostics_.duplicate_seq_events;
        record_error(line_number, event.seq_num, res.error, res.error_detail);
        return res;
    }
    if (event.symbol.empty()) {
        res.error = ValidationErrorCode::SYMBOL_MISMATCH;
        res.error_detail = "symbol must be non-empty";
        record_error(line_number, event.seq_num, res.error, res.error_detail);
        return res;
    }
    if (has_seen_event_ && event.symbol != stream_symbol_) {
        res.error = ValidationErrorCode::SYMBOL_MISMATCH;
        res.error_detail = "symbol '" + event.symbol + "' does not match stream symbol '" + stream_symbol_ + "'";
        record_error(line_number, event.seq_num, res.error, res.error_detail);
        return res;
    }
    if (has_seen_event_ && event.seq_num <= last_seq_num_) {
        res.error = ValidationErrorCode::DUPLICATE_OR_REGRESSING_SEQ;
        res.error_detail = "seq_num " + std::to_string(event.seq_num) + " <= last_seq_num " + std::to_string(last_seq_num_);
        ++diagnostics_.duplicate_seq_events;
        record_error(line_number, event.seq_num, res.error, res.error_detail);
        return res;
    }
    if (has_seen_event_ && event.event_ts_ns < last_event_ts_ns_) {
        res.error = ValidationErrorCode::OUT_OF_ORDER_TS;
        res.error_detail = "event_ts_ns regressed relative to previous accepted event";
        ++diagnostics_.out_of_order_events;
        record_error(line_number, event.seq_num, res.error, res.error_detail);
        return res;
    }

    // Event-type field validation.
    switch (event.type) {
        case EventType::L2_SNAPSHOT_CLEAR:
        case EventType::HEARTBEAT:
            break;
        case EventType::L2_SET:
            if (event.side == Side::UNKNOWN) {
                res.error = ValidationErrorCode::INVALID_SIDE;
                res.error_detail = "L2_SET requires BID or ASK side";
                record_error(line_number, event.seq_num, res.error, res.error_detail);
                return res;
            }
            if (event.price_ticks <= 0) {
                res.error = ValidationErrorCode::INVALID_PRICE;
                res.error_detail = "L2_SET requires price_ticks > 0";
                record_error(line_number, event.seq_num, res.error, res.error_detail);
                return res;
            }
            if (event.qty < 0) {
                res.error = ValidationErrorCode::INVALID_QTY;
                res.error_detail = "L2_SET requires qty >= 0";
                record_error(line_number, event.seq_num, res.error, res.error_detail);
                return res;
            }
            break;
        case EventType::L3_ADD:
            if (event.side == Side::UNKNOWN) {
                res.error = ValidationErrorCode::INVALID_SIDE;
                res.error_detail = "L3_ADD requires BID or ASK side";
                record_error(line_number, event.seq_num, res.error, res.error_detail);
                return res;
            }
            if (event.price_ticks <= 0) {
                res.error = ValidationErrorCode::INVALID_PRICE;
                res.error_detail = "L3_ADD requires price_ticks > 0";
                record_error(line_number, event.seq_num, res.error, res.error_detail);
                return res;
            }
            if (event.qty <= 0) {
                res.error = ValidationErrorCode::INVALID_QTY;
                res.error_detail = "L3_ADD requires qty > 0";
                record_error(line_number, event.seq_num, res.error, res.error_detail);
                return res;
            }
            if (event.order_id == 0) {
                res.error = ValidationErrorCode::INVALID_ORDER_ID;
                res.error_detail = "L3_ADD requires order_id > 0";
                record_error(line_number, event.seq_num, res.error, res.error_detail);
                return res;
            }
            break;
        case EventType::L3_MODIFY:
            if (event.price_ticks <= 0) {
                res.error = ValidationErrorCode::INVALID_PRICE;
                res.error_detail = "L3_MODIFY requires price_ticks > 0";
                record_error(line_number, event.seq_num, res.error, res.error_detail);
                return res;
            }
            if (event.qty < 0) {
                res.error = ValidationErrorCode::INVALID_QTY;
                res.error_detail = "L3_MODIFY requires qty >= 0";
                record_error(line_number, event.seq_num, res.error, res.error_detail);
                return res;
            }
            if (event.order_id == 0) {
                res.error = ValidationErrorCode::INVALID_ORDER_ID;
                res.error_detail = "L3_MODIFY requires order_id > 0";
                record_error(line_number, event.seq_num, res.error, res.error_detail);
                return res;
            }
            break;
        case EventType::L3_CANCEL:
            if (event.qty < 0) {
                res.error = ValidationErrorCode::INVALID_QTY;
                res.error_detail = "L3_CANCEL requires qty >= 0";
                record_error(line_number, event.seq_num, res.error, res.error_detail);
                return res;
            }
            if (event.order_id == 0) {
                res.error = ValidationErrorCode::INVALID_ORDER_ID;
                res.error_detail = "L3_CANCEL requires order_id > 0";
                record_error(line_number, event.seq_num, res.error, res.error_detail);
                return res;
            }
            break;
        case EventType::L3_EXECUTE:
            if (event.qty <= 0) {
                res.error = ValidationErrorCode::INVALID_QTY;
                res.error_detail = "L3_EXECUTE requires qty > 0";
                record_error(line_number, event.seq_num, res.error, res.error_detail);
                return res;
            }
            if (event.order_id == 0) {
                res.error = ValidationErrorCode::INVALID_ORDER_ID;
                res.error_detail = "L3_EXECUTE requires order_id > 0";
                record_error(line_number, event.seq_num, res.error, res.error_detail);
                return res;
            }
            break;
        case EventType::TRADE:
            if (event.price_ticks <= 0) {
                res.error = ValidationErrorCode::INVALID_PRICE;
                res.error_detail = "TRADE requires price_ticks > 0";
                record_error(line_number, event.seq_num, res.error, res.error_detail);
                return res;
            }
            if (event.qty <= 0) {
                res.error = ValidationErrorCode::INVALID_QTY;
                res.error_detail = "TRADE requires qty > 0";
                record_error(line_number, event.seq_num, res.error, res.error_detail);
                return res;
            }
            break;
    }

    // 2. Capture pre-event book snapshot and apply book transition if applicable.
    TopOfBook pre_tob = book_->top_of_book(config_.depth_levels_k);
    QueueSnapshot exec_queue{};
    BookErrorCode book_ec = BookErrorCode::OK;

    switch (event.type) {
        case EventType::L2_SNAPSHOT_CLEAR:
            book_->clear(event.side);
            break;
        case EventType::L2_SET:
            book_ec = book_->set_l2_level(event.side, event.price_ticks, event.qty);
            break;
        case EventType::L3_ADD:
            book_ec = book_->add_l3_order(event.order_id, event.side, event.price_ticks, event.qty);
            break;
        case EventType::L3_MODIFY:
            book_ec = book_->modify_l3_order(event.order_id, event.price_ticks, event.qty);
            break;
        case EventType::L3_CANCEL:
            book_ec = book_->cancel_l3_order(event.order_id, event.qty);
            break;
        case EventType::L3_EXECUTE:
            book_ec = book_->execute_l3_order(event.order_id, event.qty, exec_queue);
            break;
        case EventType::TRADE:
        case EventType::HEARTBEAT:
            break;
    }

    if (book_ec != BookErrorCode::OK) {
        res.error = map_book_error(book_ec);
        res.error_detail = std::string("Book rejected event: ") + to_string(res.error);
        if (book_ec == BookErrorCode::CAPACITY_LEVELS_EXCEEDED ||
            book_ec == BookErrorCode::CAPACITY_ORDERS_EXCEEDED) {
            ++diagnostics_.capacity_errors;
        } else if (book_ec == BookErrorCode::ORDER_NOT_FOUND) {
            ++diagnostics_.unknown_order_errors;
        }
        record_error(line_number, event.seq_num, res.error, res.error_detail);
        return res;
    }

    // 3. Event accepted — update sequence/gap tracking strictly causally.
    bool gap_detected_here = false;
    if (!has_seen_event_) {
        has_seen_event_ = true;
        stream_symbol_ = event.symbol;
        diagnostics_.symbol = stream_symbol_;
    } else if (event.seq_num > last_seq_num_ + 1) {
        gap_detected_here = true;
        gap_active_ = true;
        has_prev_valid_tob_ = false;
        ++diagnostics_.gap_count;
        diagnostics_.gaps.push_back(GapRecord{
            last_seq_num_ + 1,
            event.seq_num,
            event.seq_num - last_seq_num_ - 1,
            last_event_ts_ns_,
            event.event_ts_ns,
            false,
            0
        });
    }
    last_seq_num_ = event.seq_num;
    last_event_ts_ns_ = event.event_ts_ns;
    ++diagnostics_.accepted_events;

    // Snapshot clear resolves any open gap window.
    if (event.type == EventType::L2_SNAPSHOT_CLEAR) {
        has_prev_valid_tob_ = false;
        if (config_.clear_gap_on_snapshot && gap_active_) {
            gap_active_ = false;
            for (auto& g : diagnostics_.gaps) {
                if (!g.resolved_by_snapshot) {
                    g.resolved_by_snapshot = true;
                    g.resolved_seq = event.seq_num;
                }
            }
        }
    }

    // Check pre-event staleness BEFORE updating quote timestamp (critical for TRADE classification).
    bool pre_is_stale = !has_quote_ts_ ||
                        ((event.event_ts_ns - last_quote_ts_ns_) > config_.stale_threshold_ns);

    bool is_quote_update = (event.type == EventType::L2_SNAPSHOT_CLEAR ||
                            event.type == EventType::L2_SET ||
                            event.type == EventType::L3_ADD ||
                            event.type == EventType::L3_MODIFY ||
                            event.type == EventType::L3_CANCEL ||
                            event.type == EventType::L3_EXECUTE);
    if (is_quote_update) {
        has_quote_ts_ = true;
        last_quote_ts_ns_ = event.event_ts_ns;
    }

    // Track L3 counters.
    if (event.type == EventType::L3_ADD) {
        ++diagnostics_.l3_adds;
        ++cumulative_adds_;
    } else if (event.type == EventType::L3_MODIFY) {
        ++diagnostics_.l3_modifies;
    } else if (event.type == EventType::L3_CANCEL) {
        ++diagnostics_.l3_cancels;
        ++cumulative_cancels_;
    } else if (event.type == EventType::L3_EXECUTE) {
        ++diagnostics_.l3_executes;
    }

    // 4. Prune rolling causal windows to [event_ts_ns - flow_window_ns, event_ts_ns].
    prune_windows(event.event_ts_ns);

    // 5. Populate causal FeatureRow.
    FeatureRow& row = res.feature;
    row.seq_num = event.seq_num;
    row.event_ts_ns = event.event_ts_ns;
    row.recv_ts_ns = event.recv_ts_ns;
    row.symbol = event.symbol;
    row.event_type = event.type;
    row.gap_active = gap_active_;
    row.gap_detected_here = gap_detected_here;

    if (gap_active_) {
        ++diagnostics_.events_in_gap_state;
    }

    TopOfBook post_tob = book_->top_of_book(config_.depth_levels_k);
    bool post_is_stale = !has_quote_ts_ ||
                         ((event.event_ts_ns - last_quote_ts_ns_) > config_.stale_threshold_ns);
    bool is_crossed = post_tob.has_bid && post_tob.has_ask && (post_tob.best_bid_ticks > post_tob.best_ask_ticks);
    bool is_locked = post_tob.has_bid && post_tob.has_ask && (post_tob.best_bid_ticks == post_tob.best_ask_ticks);
    bool book_valid = post_tob.has_bid && post_tob.has_ask && !is_crossed && !is_locked && !post_is_stale && !gap_active_;

    row.book_valid = book_valid;
    row.is_crossed = is_crossed;
    row.is_locked = is_locked;
    row.is_stale = post_is_stale;

    if (is_crossed) ++diagnostics_.crossed_book_events;
    if (is_locked) ++diagnostics_.locked_book_events;
    if (post_is_stale) ++diagnostics_.stale_book_events;

    row.has_bid = post_tob.has_bid;
    row.has_ask = post_tob.has_ask;
    row.best_bid_ticks = post_tob.best_bid_ticks;
    row.best_ask_ticks = post_tob.best_ask_ticks;
    row.bid_qty_l1 = post_tob.bid_qty_l1;
    row.ask_qty_l1 = post_tob.ask_qty_l1;
    row.bid_depth_k = post_tob.bid_depth_k;
    row.ask_depth_k = post_tob.ask_depth_k;
    row.bid_l1_order_count = post_tob.bid_l1_orders;
    row.ask_l1_order_count = post_tob.ask_l1_orders;

    if (post_tob.has_bid) {
        row.best_bid_px = static_cast<double>(post_tob.best_bid_ticks) * config_.tick_size;
    }
    if (post_tob.has_ask) {
        row.best_ask_px = static_cast<double>(post_tob.best_ask_ticks) * config_.tick_size;
    }

    if (post_tob.has_bid && post_tob.has_ask) {
        row.spread_ticks = post_tob.best_ask_ticks - post_tob.best_bid_ticks;
        row.spread_abs = static_cast<double>(row.spread_ticks) * config_.tick_size;
        row.mid_price = 0.5 * (row.best_bid_px + row.best_ask_px);
        if (row.mid_price > 0.0) {
            row.spread_bps = (row.spread_abs / row.mid_price) * 10000.0;
        }

        int64_t l1_sum = post_tob.bid_qty_l1 + post_tob.ask_qty_l1;
        if (l1_sum > 0) {
            row.obi_l1 = static_cast<double>(post_tob.bid_qty_l1 - post_tob.ask_qty_l1) /
                         static_cast<double>(l1_sum);
            row.microprice = (row.best_bid_px * static_cast<double>(post_tob.ask_qty_l1) +
                              row.best_ask_px * static_cast<double>(post_tob.bid_qty_l1)) /
                             static_cast<double>(l1_sum);
            row.microprice_offset_ticks = (row.microprice - row.mid_price) / config_.tick_size;
        }
        int64_t k_sum = post_tob.bid_depth_k + post_tob.ask_depth_k;
        if (k_sum > 0) {
            row.obi_k = static_cast<double>(post_tob.bid_depth_k - post_tob.ask_depth_k) /
                        static_cast<double>(k_sum);
        }
    }

    // Top-of-book depletion metrics (strictly causal pre -> post diff).
    if (is_quote_update && event.type != EventType::L2_SNAPSHOT_CLEAR) {
        if (pre_tob.has_bid) {
            if (!post_tob.has_bid || post_tob.best_bid_ticks < pre_tob.best_bid_ticks) {
                row.top_bid_depletion_qty = pre_tob.bid_qty_l1;
            } else if (post_tob.best_bid_ticks == pre_tob.best_bid_ticks && post_tob.bid_qty_l1 < pre_tob.bid_qty_l1) {
                row.top_bid_depletion_qty = pre_tob.bid_qty_l1 - post_tob.bid_qty_l1;
            }
        }
        if (pre_tob.has_ask) {
            if (!post_tob.has_ask || post_tob.best_ask_ticks > pre_tob.best_ask_ticks) {
                row.top_ask_depletion_qty = pre_tob.ask_qty_l1;
            } else if (post_tob.best_ask_ticks == pre_tob.best_ask_ticks && post_tob.ask_qty_l1 < pre_tob.ask_qty_l1) {
                row.top_ask_depletion_qty = pre_tob.ask_qty_l1 - post_tob.ask_qty_l1;
            }
        }
    }

    // Cont-Kukanov-Stoikov incremental OFI on valid contiguous quote transitions.
    if (book_valid && has_prev_valid_tob_ && is_quote_update && event.type != EventType::L2_SNAPSHOT_CLEAR) {
        row.ofi_event = compute_event_ofi(prev_valid_tob_, post_tob);
        ofi_window_.push_back(TimedOfi{event.event_ts_ns, row.ofi_event});
        rolling_ofi_sum_ += row.ofi_event;
    }
    row.ofi_window = rolling_ofi_sum_;

    if (book_valid) {
        prev_valid_tob_ = post_tob;
        has_prev_valid_tob_ = true;
    } else if (is_crossed || is_locked || gap_active_) {
        has_prev_valid_tob_ = false;
    }

    // Queue metrics on L3_EXECUTE.
    if (event.type == EventType::L3_EXECUTE && exec_queue.found) {
        row.exec_queue_ahead_qty = exec_queue.queue_ahead_qty;
        row.exec_queue_ahead_orders = static_cast<int64_t>(exec_queue.queue_ahead_orders);
        row.exec_queue_position_ratio = exec_queue.queue_position_ratio;
        if (exec_queue.queue_ahead_qty > 0) {
            row.fifo_anomaly = true;
            ++diagnostics_.fifo_anomalies;
        }
    }
    if (cumulative_adds_ > 0) {
        row.cancel_to_add_ratio = static_cast<double>(cumulative_cancels_) /
                                  static_cast<double>(cumulative_adds_);
    }

    // 6. Causal Trade Classification with explicit ambiguity (strictly pre-trade state).
    if (event.type == EventType::TRADE) {
        row.is_trade = true;
        row.trade_price_ticks = event.price_ticks;
        row.trade_price = static_cast<double>(event.price_ticks) * config_.tick_size;
        row.trade_qty = event.qty;
        ++diagnostics_.total_trades;

        // Tick-rule diagnostic hint (never silently overrides AMBIGUOUS primary sign).
        if (has_prev_trade_) {
            if (event.price_ticks > prev_trade_price_ticks_) {
                last_nonzero_tick_dir_ = 1;
            } else if (event.price_ticks < prev_trade_price_ticks_) {
                last_nonzero_tick_dir_ = -1;
            }
            row.tick_rule_hint = last_nonzero_tick_dir_;
        } else {
            row.tick_rule_hint = 0;
        }
        has_prev_trade_ = true;
        prev_trade_price_ticks_ = event.price_ticks;

        bool pre_crossed = pre_tob.has_bid && pre_tob.has_ask && (pre_tob.best_bid_ticks > pre_tob.best_ask_ticks);
        bool pre_locked = pre_tob.has_bid && pre_tob.has_ask && (pre_tob.best_bid_ticks == pre_tob.best_ask_ticks);

        if (gap_active_) {
            row.trade_sign = TradeSign::AMBIGUOUS;
            row.classification_reason = ClassificationReason::GAP_ACTIVE;
            row.is_ambiguous = true;
            ++diagnostics_.ambiguous_trades;
            ++diagnostics_.ambiguous_gap;
        } else if (!pre_tob.has_bid || !pre_tob.has_ask) {
            row.trade_sign = TradeSign::AMBIGUOUS;
            row.classification_reason = ClassificationReason::BOOK_INCOMPLETE;
            row.is_ambiguous = true;
            ++diagnostics_.ambiguous_trades;
            ++diagnostics_.ambiguous_book_invalid;
        } else if (pre_crossed) {
            row.trade_sign = TradeSign::AMBIGUOUS;
            row.classification_reason = ClassificationReason::BOOK_CROSSED;
            row.is_ambiguous = true;
            ++diagnostics_.ambiguous_trades;
            ++diagnostics_.ambiguous_book_invalid;
        } else if (pre_locked) {
            row.trade_sign = TradeSign::AMBIGUOUS;
            row.classification_reason = ClassificationReason::BOOK_LOCKED;
            row.is_ambiguous = true;
            ++diagnostics_.ambiguous_trades;
            ++diagnostics_.ambiguous_book_invalid;
        } else if (pre_is_stale) {
            row.trade_sign = TradeSign::AMBIGUOUS;
            row.classification_reason = ClassificationReason::BOOK_STALE;
            row.is_ambiguous = true;
            ++diagnostics_.ambiguous_trades;
            ++diagnostics_.ambiguous_stale;
        } else {
            int64_t twice_px = 2 * event.price_ticks;
            int64_t bid_plus_ask = pre_tob.best_bid_ticks + pre_tob.best_ask_ticks;
            if (event.price_ticks >= pre_tob.best_ask_ticks) {
                row.trade_sign = TradeSign::BUY;
                row.classification_reason = ClassificationReason::QUOTE_AT_OR_ABOVE_ASK;
                ++diagnostics_.classified_buys;
            } else if (event.price_ticks <= pre_tob.best_bid_ticks) {
                row.trade_sign = TradeSign::SELL;
                row.classification_reason = ClassificationReason::QUOTE_AT_OR_BELOW_BID;
                ++diagnostics_.classified_sells;
            } else if (twice_px > bid_plus_ask) {
                row.trade_sign = TradeSign::BUY;
                row.classification_reason = ClassificationReason::QUOTE_ABOVE_MID;
                ++diagnostics_.classified_buys;
            } else if (twice_px < bid_plus_ask) {
                row.trade_sign = TradeSign::SELL;
                row.classification_reason = ClassificationReason::QUOTE_BELOW_MID;
                ++diagnostics_.classified_sells;
            } else {
                row.trade_sign = TradeSign::AMBIGUOUS;
                row.classification_reason = ClassificationReason::MIDPOINT_AMBIGUOUS;
                row.is_ambiguous = true;
                ++diagnostics_.ambiguous_trades;
                ++diagnostics_.ambiguous_midpoint;
            }
        }

        int64_t sign_val = static_cast<int64_t>(row.trade_sign);
        row.signed_qty = (row.trade_sign == TradeSign::BUY || row.trade_sign == TradeSign::SELL)
                             ? (sign_val * event.qty)
                             : 0;
        int64_t ambig_qty = row.is_ambiguous ? event.qty : 0;

        trade_window_.push_back(TimedTradeFlow{event.event_ts_ns, row.signed_qty, event.qty, ambig_qty});
        rolling_signed_flow_qty_ += row.signed_qty;
        rolling_total_trade_qty_ += event.qty;
        rolling_ambiguous_qty_ += ambig_qty;
    }

    row.rolling_signed_flow_qty = rolling_signed_flow_qty_;
    row.rolling_total_trade_qty = rolling_total_trade_qty_;
    row.rolling_ambiguous_qty = rolling_ambiguous_qty_;
    int64_t classified_qty = rolling_total_trade_qty_ - rolling_ambiguous_qty_;
    if (classified_qty > 0) {
        row.rolling_trade_flow_imbalance =
            static_cast<double>(rolling_signed_flow_qty_) / static_cast<double>(classified_qty);
    }

    res.accepted = true;
    res.error = ValidationErrorCode::NONE;
    return res;
}

}  // namespace mmae

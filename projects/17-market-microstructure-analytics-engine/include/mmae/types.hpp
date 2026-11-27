#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace mmae {

inline constexpr const char* kSchemaVersion = "mmae.v1";

enum class Side : int8_t {
    UNKNOWN = 0,
    BID = 1,
    ASK = -1
};

enum class EventType : uint8_t {
    L2_SNAPSHOT_CLEAR = 0,
    L2_SET = 1,
    L3_ADD = 2,
    L3_MODIFY = 3,
    L3_CANCEL = 4,
    L3_EXECUTE = 5,
    TRADE = 6,
    HEARTBEAT = 7
};

enum class TradeSign : int8_t {
    SELL = -1,
    AMBIGUOUS = 0,
    BUY = 1,
    NONE = -99
};

enum class ClassificationReason : uint8_t {
    NOT_A_TRADE = 0,
    QUOTE_AT_OR_ABOVE_ASK = 1,
    QUOTE_AT_OR_BELOW_BID = 2,
    QUOTE_ABOVE_MID = 3,
    QUOTE_BELOW_MID = 4,
    MIDPOINT_AMBIGUOUS = 5,
    BOOK_INCOMPLETE = 6,
    BOOK_CROSSED = 7,
    BOOK_LOCKED = 8,
    BOOK_STALE = 9,
    GAP_ACTIVE = 10
};

enum class ValidationErrorCode : uint8_t {
    NONE = 0,
    MALFORMED_ROW = 1,
    UNKNOWN_EVENT_TYPE = 2,
    INVALID_SIDE = 3,
    INVALID_TIMESTAMP = 4,
    OUT_OF_ORDER_TS = 5,
    DUPLICATE_OR_REGRESSING_SEQ = 6,
    SYMBOL_MISMATCH = 7,
    INVALID_PRICE = 8,
    INVALID_QTY = 9,
    INVALID_ORDER_ID = 10,
    BOOK_CAPACITY_LEVELS_EXCEEDED = 11,
    BOOK_CAPACITY_ORDERS_EXCEEDED = 12,
    BOOK_DUPLICATE_ORDER_ID = 13,
    BOOK_ORDER_NOT_FOUND = 14
};

struct Event {
    int64_t event_ts_ns = 0;
    int64_t recv_ts_ns = 0;
    uint64_t seq_num = 0;
    std::string symbol;
    EventType type = EventType::HEARTBEAT;
    Side side = Side::UNKNOWN;
    int64_t price_ticks = 0;
    int64_t qty = 0;
    uint64_t order_id = 0;
};

struct EngineConfig {
    double tick_size = 0.01;
    int64_t stale_threshold_ns = 1'000'000'000LL;  // 1 second
    int64_t flow_window_ns = 1'000'000'000LL;      // 1 second rolling causal window
    size_t depth_levels_k = 5;
    size_t max_levels = 1024;
    size_t max_orders = 65536;
    bool clear_gap_on_snapshot = true;
    std::string variant = "optimized";             // "baseline" or "optimized"
    size_t max_recorded_errors = 200;
};

struct FeatureRow {
    uint64_t seq_num = 0;
    int64_t event_ts_ns = 0;
    int64_t recv_ts_ns = 0;
    std::string symbol;
    EventType event_type = EventType::HEARTBEAT;

    bool book_valid = false;
    bool is_crossed = false;
    bool is_locked = false;
    bool is_stale = false;
    bool gap_active = false;
    bool gap_detected_here = false;

    bool has_bid = false;
    bool has_ask = false;
    int64_t best_bid_ticks = 0;
    int64_t best_ask_ticks = 0;
    double best_bid_px = std::numeric_limits<double>::quiet_NaN();
    double best_ask_px = std::numeric_limits<double>::quiet_NaN();
    double mid_price = std::numeric_limits<double>::quiet_NaN();
    int64_t spread_ticks = 0;
    double spread_abs = std::numeric_limits<double>::quiet_NaN();
    double spread_bps = std::numeric_limits<double>::quiet_NaN();
    double microprice = std::numeric_limits<double>::quiet_NaN();
    double microprice_offset_ticks = std::numeric_limits<double>::quiet_NaN();

    int64_t bid_qty_l1 = 0;
    int64_t ask_qty_l1 = 0;
    int64_t bid_depth_k = 0;
    int64_t ask_depth_k = 0;
    double obi_l1 = std::numeric_limits<double>::quiet_NaN();
    double obi_k = std::numeric_limits<double>::quiet_NaN();
    double ofi_event = 0.0;
    double ofi_window = 0.0;

    bool is_trade = false;
    int64_t trade_price_ticks = 0;
    double trade_price = std::numeric_limits<double>::quiet_NaN();
    int64_t trade_qty = 0;
    TradeSign trade_sign = TradeSign::NONE;
    ClassificationReason classification_reason = ClassificationReason::NOT_A_TRADE;
    bool is_ambiguous = false;
    int8_t tick_rule_hint = 0;
    int64_t signed_qty = 0;

    int64_t rolling_signed_flow_qty = 0;
    int64_t rolling_total_trade_qty = 0;
    int64_t rolling_ambiguous_qty = 0;
    double rolling_trade_flow_imbalance = std::numeric_limits<double>::quiet_NaN();

    size_t bid_l1_order_count = 0;
    size_t ask_l1_order_count = 0;
    int64_t exec_queue_ahead_qty = -1;
    int64_t exec_queue_ahead_orders = -1;
    double exec_queue_position_ratio = std::numeric_limits<double>::quiet_NaN();
    bool fifo_anomaly = false;
    int64_t top_bid_depletion_qty = 0;
    int64_t top_ask_depletion_qty = 0;
    double cancel_to_add_ratio = std::numeric_limits<double>::quiet_NaN();
};

struct GapRecord {
    uint64_t expected_seq = 0;
    uint64_t observed_seq = 0;
    uint64_t missing_count = 0;
    int64_t prev_event_ts_ns = 0;
    int64_t observed_event_ts_ns = 0;
    bool resolved_by_snapshot = false;
    uint64_t resolved_seq = 0;
};

struct ValidationErrorRecord {
    size_t line_number = 0;
    uint64_t seq_num = 0;
    ValidationErrorCode code = ValidationErrorCode::NONE;
    std::string detail;
};

struct Diagnostics {
    std::string schema_version = kSchemaVersion;
    std::string engine_variant;
    std::string symbol;
    double tick_size = 0.01;
    int64_t stale_threshold_ns = 0;
    int64_t flow_window_ns = 0;
    size_t depth_levels_k = 0;
    size_t max_levels = 0;
    size_t max_orders = 0;

    uint64_t total_input_rows = 0;
    uint64_t accepted_events = 0;
    uint64_t rejected_events = 0;
    uint64_t out_of_order_events = 0;
    uint64_t duplicate_seq_events = 0;
    uint64_t capacity_errors = 0;
    uint64_t unknown_order_errors = 0;

    uint64_t crossed_book_events = 0;
    uint64_t locked_book_events = 0;
    uint64_t stale_book_events = 0;
    uint64_t gap_count = 0;
    uint64_t events_in_gap_state = 0;

    uint64_t total_trades = 0;
    uint64_t classified_buys = 0;
    uint64_t classified_sells = 0;
    uint64_t ambiguous_trades = 0;
    uint64_t ambiguous_midpoint = 0;
    uint64_t ambiguous_book_invalid = 0;
    uint64_t ambiguous_stale = 0;
    uint64_t ambiguous_gap = 0;

    uint64_t l3_adds = 0;
    uint64_t l3_modifies = 0;
    uint64_t l3_cancels = 0;
    uint64_t l3_executes = 0;
    uint64_t fifo_anomalies = 0;

    std::vector<GapRecord> gaps;
    std::vector<ValidationErrorRecord> errors;
};

const char* to_string(Side side);
const char* to_string(EventType type);
const char* to_string(TradeSign sign);
const char* to_string(ClassificationReason reason);
const char* to_string(ValidationErrorCode code);

bool parse_side(const std::string& s, Side& out);
bool parse_event_type(const std::string& s, EventType& out);
bool validate_config(const EngineConfig& cfg, std::string& error_out);

}  // namespace mmae

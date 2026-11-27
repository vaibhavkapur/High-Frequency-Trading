// SPDX-License-Identifier: MIT
// FPGA Order Book Accelerator — Shared Binary Interface & Types Package
// Schema version: fpga-ob.v1 (see docs/contracts.md)

package order_book_pkg;

    localparam logic [7:0]  SCHEMA_VERSION    = 8'h01;
    localparam int unsigned DEFAULT_PRICE_BASE = 32'd10000;
    localparam int unsigned DEFAULT_NUM_LEVELS = 64;
    localparam int unsigned DEFAULT_MAX_ACTIVE = 32;

    typedef enum logic [7:0] {
        OP_NOP     = 8'h00,
        OP_ADD     = 8'h01,
        OP_CANCEL  = 8'h02,
        OP_EXECUTE = 8'h03
    } opcode_e;

    typedef enum logic [7:0] {
        SIDE_BID = 8'h00,
        SIDE_ASK = 8'h01
    } side_e;

    typedef enum logic [7:0] {
        STATUS_OK              = 8'h00,
        ERR_UNSUPPORTED_SCHEMA = 8'h01,
        ERR_UNSUPPORTED_FLAGS  = 8'h02,
        ERR_UNSUPPORTED_OPCODE = 8'h03,
        ERR_INVALID_SIDE       = 8'h04,
        ERR_PRICE_OOB          = 8'h05,
        ERR_ZERO_QTY           = 8'h06,
        ERR_FULL_CAPACITY      = 8'h07,
        ERR_OVERFLOW_QTY       = 8'h08,
        ERR_OVERFLOW_ORDERS    = 8'h09,
        ERR_INVALID_CANCEL     = 8'h0A,
        ERR_INVALID_EXECUTE    = 8'h0B
    } status_e;

    // 160-bit fixed-width ingress event packet
    typedef struct packed {
        logic [31:0] seq_no;
        logic [31:0] timestamp_ns;
        logic [31:0] price;
        logic [31:0] qty;
        logic [7:0]  opcode;
        logic [7:0]  side;
        logic [7:0]  flags;
        logic [7:0]  schema_ver;
    } event_pkt_t;

    // Output BBO snapshot and status packet
    typedef struct packed {
        logic [7:0]         schema_ver;
        logic               lane1_valid;
        logic [7:0]         status_0;
        logic [7:0]         status_1;
        logic [31:0]        seq_no_0;
        logic [31:0]        seq_no_1;
        logic [31:0]        timestamp_ns;
        logic               best_bid_valid;
        logic [31:0]        best_bid_price;
        logic [31:0]        best_bid_qty;
        logic [15:0]        best_bid_orders;
        logic               best_ask_valid;
        logic [31:0]        best_ask_price;
        logic [31:0]        best_ask_qty;
        logic [15:0]        best_ask_orders;
        logic signed [31:0] spread;
        logic               crossed;
        logic [15:0]        active_bid_levels;
        logic [15:0]        active_ask_levels;
    } bbo_pkt_t;

    // Diagnostic telemetry counters
    typedef struct packed {
        logic [31:0] cycles_total;
        logic [31:0] events_accepted;
        logic [31:0] events_committed;
        logic [31:0] events_rejected;
        logic [31:0] backpressure_stalls;
        logic [31:0] dual_lane_cycles;
        logic [15:0] max_active_bid_levels;
        logic [15:0] max_active_ask_levels;
    } diag_counters_t;

endpackage : order_book_pkg

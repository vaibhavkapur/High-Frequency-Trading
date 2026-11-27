// SPDX-License-Identifier: MIT
// Top-Level 3-Stage Pipelined FPGA Order Book Accelerator
// Latency: 3 clock cycles (when out_ready=1)
// Initiation Interval: 1 cycle/event (0.5 cycles/event in dual-lane mode)
// Backpressure: Global synchronous stall (pipe_advance = !out_valid || out_ready)

module order_book_top
    import order_book_pkg::*;
#(
    parameter int unsigned PRICE_BASE        = DEFAULT_PRICE_BASE,
    parameter int unsigned NUM_LEVELS        = DEFAULT_NUM_LEVELS,
    parameter int unsigned MAX_ACTIVE_LEVELS = DEFAULT_MAX_ACTIVE
) (
    input  logic           clk,
    input  logic           rst_n,

    // Ingress AXI-Stream style handshake (Lane 0 primary + Lane 1 simultaneous)
    input  logic           in_valid_0,
    input  event_pkt_t     in_event_0,
    input  logic           in_valid_1,
    input  event_pkt_t     in_event_1,
    output logic           in_ready,

    // Egress BBO snapshot stream
    output logic           out_valid,
    input  logic           out_ready,
    output bbo_pkt_t       out_bbo,

    // Live diagnostic telemetry counters
    output diag_counters_t diag_o
);

    localparam int unsigned IDX_W = (NUM_LEVELS > 1) ? $clog2(NUM_LEVELS) : 1;

    // Global pipeline advance: freeze entire pipeline when output is stalled
    logic pipe_advance;
    assign pipe_advance = (!out_valid) || out_ready;
    assign in_ready     = pipe_advance;

    // -------------------------------------------------------------------------
    // Pre-validation function (Stage 1 combinational decode)
    // -------------------------------------------------------------------------
    typedef struct packed {
        logic             valid;
        logic             exec_bank;
        logic [7:0]       pre_status;
        logic [7:0]       opcode;
        logic [7:0]       side;
        logic [IDX_W-1:0] idx;
        logic [31:0]      qty;
        logic [31:0]      seq_no;
        logic [31:0]      timestamp_ns;
    } decoded_lane_t;

    function automatic decoded_lane_t decode_event(input logic v, input event_pkt_t pkt);
        decoded_lane_t d;
        logic [31:0] offset;
        d              = '0;
        d.valid        = v;
        d.opcode       = pkt.opcode;
        d.side         = pkt.side;
        d.qty          = pkt.qty;
        d.seq_no       = pkt.seq_no;
        d.timestamp_ns = pkt.timestamp_ns;
        d.pre_status   = STATUS_OK;
        d.exec_bank    = 1'b0;
        offset         = pkt.price - PRICE_BASE;

        if (!v) begin
            return d;
        end

        if (pkt.schema_ver != SCHEMA_VERSION) begin
            d.pre_status = ERR_UNSUPPORTED_SCHEMA;
        end else if (pkt.flags != 8'h00) begin
            d.pre_status = ERR_UNSUPPORTED_FLAGS;
        end else if ((pkt.opcode != OP_NOP) &&
                     (pkt.opcode != OP_ADD) &&
                     (pkt.opcode != OP_CANCEL) &&
                     (pkt.opcode != OP_EXECUTE)) begin
            d.pre_status = ERR_UNSUPPORTED_OPCODE;
        end else if (pkt.opcode == OP_NOP) begin
            d.pre_status = STATUS_OK;
            d.exec_bank  = 1'b0;
        end else if ((pkt.side != SIDE_BID) && (pkt.side != SIDE_ASK)) begin
            d.pre_status = ERR_INVALID_SIDE;
        end else if ((pkt.price < PRICE_BASE) || (pkt.price >= (PRICE_BASE + NUM_LEVELS))) begin
            d.pre_status = ERR_PRICE_OOB;
        end else if (pkt.qty == 32'd0) begin
            d.pre_status = ERR_ZERO_QTY;
        end else begin
            d.pre_status = STATUS_OK;
            d.exec_bank  = 1'b1;
            d.idx        = offset[IDX_W-1:0];
        end
        return d;
    endfunction

    // -------------------------------------------------------------------------
    // Stage 1 Registers (Ingress Decode)
    // -------------------------------------------------------------------------
    logic          s1_active;
    decoded_lane_t s1_lane0;
    decoded_lane_t s1_lane1;

    decoded_lane_t dec0_comb, dec1_comb;
    always_comb begin
        dec0_comb = decode_event(in_valid_0, in_event_0);
        // Lane 1 only accepted if Lane 0 is also valid in this cycle
        dec1_comb = decode_event(in_valid_0 && in_valid_1, in_event_1);
    end

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            s1_active <= 1'b0;
            s1_lane0  <= '0;
            s1_lane1  <= '0;
        end else if (pipe_advance) begin
            s1_active <= in_valid_0;
            s1_lane0  <= dec0_comb;
            s1_lane1  <= dec1_comb;
        end
    end

    // -------------------------------------------------------------------------
    // Stage 2: Dual Price-Level Banks (Bid & Ask) + Stage 2 Metadata Registers
    // -------------------------------------------------------------------------
    logic bid_req0_en, bid_req1_en;
    logic ask_req0_en, ask_req1_en;
    logic [7:0] bid_resp0_st, bid_resp1_st;
    logic [7:0] ask_resp0_st, ask_resp1_st;

    assign bid_req0_en = s1_active && s1_lane0.exec_bank && (s1_lane0.side == SIDE_BID);
    assign bid_req1_en = s1_active && s1_lane1.valid && s1_lane1.exec_bank && (s1_lane1.side == SIDE_BID);
    assign ask_req0_en = s1_active && s1_lane0.exec_bank && (s1_lane0.side == SIDE_ASK);
    assign ask_req1_en = s1_active && s1_lane1.valid && s1_lane1.exec_bank && (s1_lane1.side == SIDE_ASK);

    logic [NUM_LEVELS-1:0] bid_active_mask, ask_active_mask;
    logic [15:0]           bid_active_count, ask_active_count;
    logic                  best_bid_valid, best_ask_valid;
    logic [IDX_W-1:0]      best_bid_idx, best_ask_idx;
    logic [31:0]           best_bid_qty_w, best_ask_qty_w;
    logic [15:0]           best_bid_ord_w, best_ask_ord_w;

    price_level_bank #(
        .NUM_LEVELS        (NUM_LEVELS),
        .MAX_ACTIVE_LEVELS (MAX_ACTIVE_LEVELS)
    ) u_bid_bank (
        .clk            (clk),
        .rst_n          (rst_n),
        .advance        (pipe_advance),
        .req0_en        (bid_req0_en),
        .req0_opcode    (s1_lane0.opcode),
        .req0_idx       (s1_lane0.idx),
        .req0_qty       (s1_lane0.qty),
        .resp0_status   (bid_resp0_st),
        .req1_en        (bid_req1_en),
        .req1_opcode    (s1_lane1.opcode),
        .req1_idx       (s1_lane1.idx),
        .req1_qty       (s1_lane1.qty),
        .resp1_status   (bid_resp1_st),
        .active_mask_o  (bid_active_mask),
        .active_count_o (bid_active_count),
        .query_idx_i    (best_bid_idx),
        .query_qty_o    (best_bid_qty_w),
        .query_orders_o (best_bid_ord_w)
    );

    price_level_bank #(
        .NUM_LEVELS        (NUM_LEVELS),
        .MAX_ACTIVE_LEVELS (MAX_ACTIVE_LEVELS)
    ) u_ask_bank (
        .clk            (clk),
        .rst_n          (rst_n),
        .advance        (pipe_advance),
        .req0_en        (ask_req0_en),
        .req0_opcode    (s1_lane0.opcode),
        .req0_idx       (s1_lane0.idx),
        .req0_qty       (s1_lane0.qty),
        .resp0_status   (ask_resp0_st),
        .req1_en        (ask_req1_en),
        .req1_opcode    (s1_lane1.opcode),
        .req1_idx       (s1_lane1.idx),
        .req1_qty       (s1_lane1.qty),
        .resp1_status   (ask_resp1_st),
        .active_mask_o  (ask_active_mask),
        .active_count_o (ask_active_count),
        .query_idx_i    (best_ask_idx),
        .query_qty_o    (best_ask_qty_w),
        .query_orders_o (best_ask_ord_w)
    );

    logic [7:0] resolved_status_0, resolved_status_1;
    always_comb begin
        if (s1_lane0.pre_status != STATUS_OK) begin
            resolved_status_0 = s1_lane0.pre_status;
        end else if (bid_req0_en) begin
            resolved_status_0 = bid_resp0_st;
        end else if (ask_req0_en) begin
            resolved_status_0 = ask_resp0_st;
        end else begin
            resolved_status_0 = STATUS_OK;
        end

        if (!s1_lane1.valid) begin
            resolved_status_1 = STATUS_OK;
        end else if (s1_lane1.pre_status != STATUS_OK) begin
            resolved_status_1 = s1_lane1.pre_status;
        end else if (bid_req1_en) begin
            resolved_status_1 = bid_resp1_st;
        end else if (ask_req1_en) begin
            resolved_status_1 = ask_resp1_st;
        end else begin
            resolved_status_1 = STATUS_OK;
        end
    end

    logic        s2_active;
    logic        s2_lane1_valid;
    logic [7:0]  s2_status_0;
    logic [7:0]  s2_status_1;
    logic [31:0] s2_seq_no_0;
    logic [31:0] s2_seq_no_1;
    logic [31:0] s2_timestamp_ns;

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            s2_active       <= 1'b0;
            s2_lane1_valid  <= 1'b0;
            s2_status_0     <= STATUS_OK;
            s2_status_1     <= STATUS_OK;
            s2_seq_no_0     <= 32'd0;
            s2_seq_no_1     <= 32'd0;
            s2_timestamp_ns <= 32'd0;
        end else if (pipe_advance) begin
            s2_active       <= s1_active;
            s2_lane1_valid  <= s1_active && s1_lane1.valid;
            s2_status_0     <= resolved_status_0;
            s2_status_1     <= resolved_status_1;
            s2_seq_no_0     <= s1_lane0.seq_no;
            s2_seq_no_1     <= s1_lane1.valid ? s1_lane1.seq_no : 32'd0;
            s2_timestamp_ns <= s1_lane1.valid ? s1_lane1.timestamp_ns : s1_lane0.timestamp_ns;
        end
    end

    // -------------------------------------------------------------------------
    // Stage 3: Combinational BBO Priority Reduction Trees + Output Register
    // -------------------------------------------------------------------------
    bbo_tree #(
        .NUM_LEVELS     (NUM_LEVELS),
        .SELECT_HIGHEST (1'b1)
    ) u_bid_bbo_tree (
        .active_mask (bid_active_mask),
        .best_valid  (best_bid_valid),
        .best_idx    (best_bid_idx)
    );

    bbo_tree #(
        .NUM_LEVELS     (NUM_LEVELS),
        .SELECT_HIGHEST (1'b0)
    ) u_ask_bbo_tree (
        .active_mask (ask_active_mask),
        .best_valid  (best_ask_valid),
        .best_idx    (best_ask_idx)
    );

    logic [31:0]        best_bid_price_w, best_ask_price_w;
    logic signed [31:0] spread_w;
    logic               crossed_w;

    always_comb begin
        best_bid_price_w = best_bid_valid ? (PRICE_BASE + {{(32-IDX_W){1'b0}}, best_bid_idx}) : 32'd0;
        best_ask_price_w = best_ask_valid ? (PRICE_BASE + {{(32-IDX_W){1'b0}}, best_ask_idx}) : 32'd0;
        if (best_bid_valid && best_ask_valid) begin
            spread_w  = $signed(best_ask_price_w) - $signed(best_bid_price_w);
            crossed_w = (best_bid_price_w >= best_ask_price_w);
        end else begin
            spread_w  = 32'sd0;
            crossed_w = 1'b0;
        end
    end

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            out_valid <= 1'b0;
            out_bbo   <= '0;
        end else if (pipe_advance) begin
            out_valid                 <= s2_active;
            out_bbo.schema_ver        <= SCHEMA_VERSION;
            out_bbo.lane1_valid       <= s2_lane1_valid;
            out_bbo.status_0          <= s2_status_0;
            out_bbo.status_1          <= s2_status_1;
            out_bbo.seq_no_0          <= s2_seq_no_0;
            out_bbo.seq_no_1          <= s2_seq_no_1;
            out_bbo.timestamp_ns      <= s2_timestamp_ns;
            out_bbo.best_bid_valid    <= best_bid_valid;
            out_bbo.best_bid_price    <= best_bid_price_w;
            out_bbo.best_bid_qty      <= best_bid_valid ? best_bid_qty_w : 32'd0;
            out_bbo.best_bid_orders   <= best_bid_valid ? best_bid_ord_w : 16'd0;
            out_bbo.best_ask_valid    <= best_ask_valid;
            out_bbo.best_ask_price    <= best_ask_price_w;
            out_bbo.best_ask_qty      <= best_ask_valid ? best_ask_qty_w : 32'd0;
            out_bbo.best_ask_orders   <= best_ask_valid ? best_ask_ord_w : 16'd0;
            out_bbo.spread            <= spread_w;
            out_bbo.crossed           <= crossed_w;
            out_bbo.active_bid_levels <= bid_active_count;
            out_bbo.active_ask_levels <= ask_active_count;
        end
    end

    // -------------------------------------------------------------------------
    // Diagnostic Telemetry Counters
    // -------------------------------------------------------------------------
    logic [31:0] commit_ok_inc;
    logic [31:0] commit_err_inc;

    always_comb begin
        commit_ok_inc  = ((s2_status_0 == STATUS_OK) ? 32'd1 : 32'd0) +
                         ((s2_lane1_valid && (s2_status_1 == STATUS_OK)) ? 32'd1 : 32'd0);
        commit_err_inc = ((s2_status_0 != STATUS_OK) ? 32'd1 : 32'd0) +
                         ((s2_lane1_valid && (s2_status_1 != STATUS_OK)) ? 32'd1 : 32'd0);
    end

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            diag_o <= '0;
        end else begin
            diag_o.cycles_total <= diag_o.cycles_total + 32'd1;

            if (in_valid_0 && !in_ready) begin
                diag_o.backpressure_stalls <= diag_o.backpressure_stalls + 32'd1;
            end

            if (in_valid_0 && in_ready) begin
                diag_o.events_accepted <= diag_o.events_accepted + (in_valid_1 ? 32'd2 : 32'd1);
                if (in_valid_1) begin
                    diag_o.dual_lane_cycles <= diag_o.dual_lane_cycles + 32'd1;
                end
            end

            if (pipe_advance && s2_active) begin
                diag_o.events_committed <= diag_o.events_committed + commit_ok_inc;
                diag_o.events_rejected  <= diag_o.events_rejected + commit_err_inc;

                if (bid_active_count > diag_o.max_active_bid_levels) begin
                    diag_o.max_active_bid_levels <= bid_active_count;
                end
                if (ask_active_count > diag_o.max_active_ask_levels) begin
                    diag_o.max_active_ask_levels <= ask_active_count;
                end
            end
        end
    end

endmodule : order_book_top

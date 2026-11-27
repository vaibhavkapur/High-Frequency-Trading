// SPDX-License-Identifier: MIT
// Bounded Direct-Indexed Price-Level Bank with Dual-Lane Atomic Update
// Evaluates Lane 0 then Lane 1 combinationally against current state so
// simultaneous updates (including same-price collisions) are bit-exact with
// sequential execution, then commits atomically on posedge clk when advance=1.

module price_level_bank
    import order_book_pkg::*;
#(
    parameter int unsigned NUM_LEVELS        = DEFAULT_NUM_LEVELS,
    parameter int unsigned MAX_ACTIVE_LEVELS = DEFAULT_MAX_ACTIVE,
    localparam int unsigned IDX_W            = (NUM_LEVELS > 1) ? $clog2(NUM_LEVELS) : 1
) (
    input  logic                  clk,
    input  logic                  rst_n,
    input  logic                  advance,

    // Lane 0 pre-validated request targeting this side
    input  logic                  req0_en,
    input  logic [7:0]            req0_opcode,
    input  logic [IDX_W-1:0]      req0_idx,
    input  logic [31:0]           req0_qty,
    output logic [7:0]            resp0_status,

    // Lane 1 pre-validated request targeting this side
    input  logic                  req1_en,
    input  logic [7:0]            req1_opcode,
    input  logic [IDX_W-1:0]      req1_idx,
    input  logic [31:0]           req1_qty,
    output logic [7:0]            resp1_status,

    // Readback ports for Stage 3 BBO extraction
    output logic [NUM_LEVELS-1:0] active_mask_o,
    output logic [15:0]           active_count_o,
    input  logic [IDX_W-1:0]      query_idx_i,
    output logic [31:0]           query_qty_o,
    output logic [15:0]           query_orders_o
);

    logic [31:0]           qty_reg          [0:NUM_LEVELS-1];
    logic [15:0]           orders_reg       [0:NUM_LEVELS-1];
    logic [NUM_LEVELS-1:0] active_mask_reg;
    logic [15:0]           active_count_reg;

    // Combinational next-state intermediates
    logic [31:0]           l0_new_qty;
    logic [15:0]           l0_new_orders;
    logic                  l0_new_active;
    logic                  l0_write_en;
    logic [15:0]           mid_active_count;

    logic [31:0]           l1_cur_qty;
    logic [15:0]           l1_cur_orders;
    logic                  l1_cur_active;
    logic [31:0]           l1_new_qty;
    logic [15:0]           l1_new_orders;
    logic                  l1_new_active;
    logic                  l1_write_en;
    logic [15:0]           next_active_count;

    // Helper combinational evaluation for a single update
    always_comb begin
        logic [31:0] cur_q0;
        logic [15:0] cur_o0;
        logic        cur_a0;
        logic [32:0] sum_q0;
        logic [32:0] sum_q1;

        // --- Lane 0 evaluation ---
        cur_q0           = qty_reg[req0_idx];
        cur_o0           = orders_reg[req0_idx];
        cur_a0           = active_mask_reg[req0_idx];

        l0_new_qty       = cur_q0;
        l0_new_orders    = cur_o0;
        l0_new_active    = cur_a0;
        l0_write_en      = 1'b0;
        mid_active_count = active_count_reg;
        resp0_status     = STATUS_OK;
        sum_q0           = 33'd0;

        if (req0_en) begin
            case (req0_opcode)
                OP_ADD: begin
                    sum_q0 = {1'b0, cur_q0} + {1'b0, req0_qty};
                    if (!cur_a0 && (active_count_reg >= MAX_ACTIVE_LEVELS[15:0])) begin
                        resp0_status = ERR_FULL_CAPACITY;
                    end else if (sum_q0[32]) begin
                        resp0_status = ERR_OVERFLOW_QTY;
                    end else if (cur_o0 == 16'hFFFF) begin
                        resp0_status = ERR_OVERFLOW_ORDERS;
                    end else begin
                        l0_new_qty    = sum_q0[31:0];
                        l0_new_orders = cur_o0 + 16'd1;
                        l0_new_active = 1'b1;
                        l0_write_en   = 1'b1;
                        if (!cur_a0) begin
                            mid_active_count = active_count_reg + 16'd1;
                        end
                    end
                end
                OP_CANCEL: begin
                    if (!cur_a0 || (req0_qty > cur_q0)) begin
                        resp0_status = ERR_INVALID_CANCEL;
                    end else begin
                        l0_new_qty  = cur_q0 - req0_qty;
                        l0_write_en = 1'b1;
                        if (l0_new_qty == 32'd0) begin
                            l0_new_orders    = 16'd0;
                            l0_new_active    = 1'b0;
                            mid_active_count = active_count_reg - 16'd1;
                        end else begin
                            l0_new_orders = (cur_o0 > 16'd1) ? (cur_o0 - 16'd1) : 16'd1;
                            l0_new_active = 1'b1;
                        end
                    end
                end
                OP_EXECUTE: begin
                    if (!cur_a0 || (req0_qty > cur_q0)) begin
                        resp0_status = ERR_INVALID_EXECUTE;
                    end else begin
                        l0_new_qty  = cur_q0 - req0_qty;
                        l0_write_en = 1'b1;
                        if (l0_new_qty == 32'd0) begin
                            l0_new_orders    = 16'd0;
                            l0_new_active    = 1'b0;
                            mid_active_count = active_count_reg - 16'd1;
                        end else begin
                            l0_new_orders = cur_o0;
                            l0_new_active = 1'b1;
                        end
                    end
                end
                default: begin
                    resp0_status = ERR_UNSUPPORTED_OPCODE;
                end
            endcase
        end

        // --- Lane 1 evaluation (forwards Lane 0 if same index) ---
        if (l0_write_en && (req1_idx == req0_idx)) begin
            l1_cur_qty    = l0_new_qty;
            l1_cur_orders = l0_new_orders;
            l1_cur_active = l0_new_active;
        end else begin
            l1_cur_qty    = qty_reg[req1_idx];
            l1_cur_orders = orders_reg[req1_idx];
            l1_cur_active = active_mask_reg[req1_idx];
        end

        l1_new_qty        = l1_cur_qty;
        l1_new_orders     = l1_cur_orders;
        l1_new_active     = l1_cur_active;
        l1_write_en       = 1'b0;
        next_active_count = mid_active_count;
        resp1_status      = STATUS_OK;
        sum_q1            = 33'd0;

        if (req1_en) begin
            case (req1_opcode)
                OP_ADD: begin
                    sum_q1 = {1'b0, l1_cur_qty} + {1'b0, req1_qty};
                    if (!l1_cur_active && (mid_active_count >= MAX_ACTIVE_LEVELS[15:0])) begin
                        resp1_status = ERR_FULL_CAPACITY;
                    end else if (sum_q1[32]) begin
                        resp1_status = ERR_OVERFLOW_QTY;
                    end else if (l1_cur_orders == 16'hFFFF) begin
                        resp1_status = ERR_OVERFLOW_ORDERS;
                    end else begin
                        l1_new_qty    = sum_q1[31:0];
                        l1_new_orders = l1_cur_orders + 16'd1;
                        l1_new_active = 1'b1;
                        l1_write_en   = 1'b1;
                        if (!l1_cur_active) begin
                            next_active_count = mid_active_count + 16'd1;
                        end
                    end
                end
                OP_CANCEL: begin
                    if (!l1_cur_active || (req1_qty > l1_cur_qty)) begin
                        resp1_status = ERR_INVALID_CANCEL;
                    end else begin
                        l1_new_qty  = l1_cur_qty - req1_qty;
                        l1_write_en = 1'b1;
                        if (l1_new_qty == 32'd0) begin
                            l1_new_orders     = 16'd0;
                            l1_new_active     = 1'b0;
                            next_active_count = mid_active_count - 16'd1;
                        end else begin
                            l1_new_orders = (l1_cur_orders > 16'd1) ? (l1_cur_orders - 16'd1) : 16'd1;
                            l1_new_active = 1'b1;
                        end
                    end
                end
                OP_EXECUTE: begin
                    if (!l1_cur_active || (req1_qty > l1_cur_qty)) begin
                        resp1_status = ERR_INVALID_EXECUTE;
                    end else begin
                        l1_new_qty  = l1_cur_qty - req1_qty;
                        l1_write_en = 1'b1;
                        if (l1_new_qty == 32'd0) begin
                            l1_new_orders     = 16'd0;
                            l1_new_active     = 1'b0;
                            next_active_count = mid_active_count - 16'd1;
                        end else begin
                            l1_new_orders = l1_cur_orders;
                            l1_new_active = 1'b1;
                        end
                    end
                end
                default: begin
                    resp1_status = ERR_UNSUPPORTED_OPCODE;
                end
            endcase
        end
    end

    // Synchronous register update with active-low reset
    integer k;
    always_ff @(posedge clk) begin
        if (!rst_n) begin
            for (k = 0; k < NUM_LEVELS; k = k + 1) begin
                qty_reg[k]    <= 32'd0;
                orders_reg[k] <= 16'd0;
            end
            active_mask_reg  <= '0;
            active_count_reg <= 16'd0;
        end else if (advance) begin
            if (l0_write_en) begin
                qty_reg[req0_idx]         <= l0_new_qty;
                orders_reg[req0_idx]      <= l0_new_orders;
                active_mask_reg[req0_idx] <= l0_new_active;
            end
            if (l1_write_en) begin
                qty_reg[req1_idx]         <= l1_new_qty;
                orders_reg[req1_idx]      <= l1_new_orders;
                active_mask_reg[req1_idx] <= l1_new_active;
            end
            active_count_reg <= next_active_count;
        end
    end

    assign active_mask_o  = active_mask_reg;
    assign active_count_o = active_count_reg;
    assign query_qty_o    = qty_reg[query_idx_i];
    assign query_orders_o = orders_reg[query_idx_i];

endmodule : price_level_bank

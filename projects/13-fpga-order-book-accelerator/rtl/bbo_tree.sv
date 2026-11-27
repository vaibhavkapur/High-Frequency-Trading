// SPDX-License-Identifier: MIT
// Parameterized O(log2(NUM_LEVELS)) combinational binary reduction tree
// Extracts highest active index (SELECT_HIGHEST=1, Best Bid) or
// lowest active index (SELECT_HIGHEST=0, Best Ask).

module bbo_tree #(
    parameter int unsigned NUM_LEVELS     = 64,
    parameter bit          SELECT_HIGHEST = 1'b1,
    localparam int unsigned IDX_W         = (NUM_LEVELS > 1) ? $clog2(NUM_LEVELS) : 1
) (
    input  logic [NUM_LEVELS-1:0] active_mask,
    output logic                  best_valid,
    output logic [IDX_W-1:0]      best_idx
);

    localparam int unsigned STAGES = (NUM_LEVELS > 1) ? $clog2(NUM_LEVELS) : 1;

    // Tree node arrays sized by power-of-two NUM_LEVELS
    logic             node_valid [0:STAGES][0:NUM_LEVELS-1];
    logic [IDX_W-1:0] node_idx   [0:STAGES][0:NUM_LEVELS-1];

    genvar i, s;
    generate
        // Leaf layer (stage 0)
        for (i = 0; i < NUM_LEVELS; i = i + 1) begin : gen_leaf
            always_comb begin
                node_valid[0][i] = active_mask[i];
                node_idx[0][i]   = i[IDX_W-1:0];
            end
        end

        // Binary reduction layers (stages 1 .. STAGES)
        for (s = 1; s <= STAGES; s = s + 1) begin : gen_stage
            localparam int unsigned NODES = NUM_LEVELS >> s;
            for (i = 0; i < NODES; i = i + 1) begin : gen_node
                always_comb begin
                    logic             lo_v, hi_v;
                    logic [IDX_W-1:0] lo_i, hi_i;
                    lo_v = node_valid[s-1][2*i];
                    lo_i = node_idx[s-1][2*i];
                    hi_v = node_valid[s-1][2*i + 1];
                    hi_i = node_idx[s-1][2*i + 1];

                    node_valid[s][i] = lo_v | hi_v;
                    if (SELECT_HIGHEST) begin
                        // Prefer higher index (Best Bid)
                        node_idx[s][i] = hi_v ? hi_i : lo_i;
                    end else begin
                        // Prefer lower index (Best Ask)
                        node_idx[s][i] = lo_v ? lo_i : hi_i;
                    end
                end
            end
        end
    endgenerate

    assign best_valid = node_valid[STAGES][0];
    assign best_idx   = node_valid[STAGES][0] ? node_idx[STAGES][0] : '0;

endmodule : bbo_tree

#include "fpga_ob/rtl_model.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace fpga_ob {

// =============================================================================
// PipelinedOrderBookAccelerator (3-stage cycle-accurate mirror of RTL)
// =============================================================================

PipelinedOrderBookAccelerator::PipelinedOrderBookAccelerator(Config cfg) : cfg_(cfg) {
    if (auto err = cfg_.validate(); err.has_value()) {
        throw std::invalid_argument("Invalid PipelinedOrderBookAccelerator config: " + *err);
    }
    hard_reset();
}

void PipelinedOrderBookAccelerator::hard_reset() {
    s1_active_ = false;
    s1_lane0_  = {};
    s1_lane1_  = {};

    bid_bank_.init(cfg_.num_levels);
    ask_bank_.init(cfg_.num_levels);

    s2_active_       = false;
    s2_lane1_valid_  = false;
    s2_status_0_     = Status::Ok;
    s2_status_1_     = Status::Ok;
    s2_seq_no_0_     = 0;
    s2_seq_no_1_     = 0;
    s2_timestamp_ns_ = 0;

    out_valid_      = false;
    out_bbo_        = {};
    last_out_ready_ = true;
    diag_           = {};
}

PipelinedOrderBookAccelerator::DecodedLane
PipelinedOrderBookAccelerator::decode_event(bool valid, const EventPacket& pkt) const {
    DecodedLane d{};
    d.valid        = valid;
    d.opcode       = pkt.opcode;
    d.side         = pkt.side;
    d.qty          = pkt.qty;
    d.seq_no       = pkt.seq_no;
    d.timestamp_ns = pkt.timestamp_ns;
    d.pre_status   = Status::Ok;
    d.exec_bank    = false;

    if (!valid) return d;

    if (pkt.schema_ver != kSchemaVersion) {
        d.pre_status = Status::ErrUnsupportedSchema;
    } else if (pkt.flags != 0) {
        d.pre_status = Status::ErrUnsupportedFlags;
    } else if (pkt.opcode > static_cast<uint8_t>(Opcode::Execute)) {
        d.pre_status = Status::ErrUnsupportedOpcode;
    } else if (pkt.opcode == static_cast<uint8_t>(Opcode::Nop)) {
        d.pre_status = Status::Ok;
        d.exec_bank  = false;
    } else if (pkt.side != static_cast<uint8_t>(Side::Bid) &&
               pkt.side != static_cast<uint8_t>(Side::Ask)) {
        d.pre_status = Status::ErrInvalidSide;
    } else if (pkt.price < cfg_.price_base || pkt.price >= cfg_.price_base + cfg_.num_levels) {
        d.pre_status = Status::ErrPriceOob;
    } else if (pkt.qty == 0) {
        d.pre_status = Status::ErrZeroQty;
    } else {
        d.pre_status = Status::Ok;
        d.exec_bank  = true;
        d.idx        = pkt.price - cfg_.price_base;
    }
    return d;
}

PipelinedOrderBookAccelerator::BankEval PipelinedOrderBookAccelerator::eval_bank(
    const BankState& bank,
    bool req0_en, uint8_t req0_op, uint32_t req0_idx, uint32_t req0_qty,
    bool req1_en, uint8_t req1_op, uint32_t req1_idx, uint32_t req1_qty) const {

    BankEval ev{};
    uint16_t mid_count = bank.active_count;

    // Lane 0 evaluation
    if (req0_en) {
        uint32_t cur_q = bank.qty[req0_idx];
        uint16_t cur_o = bank.orders[req0_idx];
        uint8_t  cur_a = bank.active[req0_idx];

        switch (static_cast<Opcode>(req0_op)) {
            case Opcode::Add: {
                uint64_t sum = static_cast<uint64_t>(cur_q) + static_cast<uint64_t>(req0_qty);
                if (!cur_a && bank.active_count >= cfg_.max_active_levels) {
                    ev.status_0 = Status::ErrFullCapacity;
                } else if (sum > std::numeric_limits<uint32_t>::max()) {
                    ev.status_0 = Status::ErrOverflowQty;
                } else if (cur_o == std::numeric_limits<uint16_t>::max()) {
                    ev.status_0 = Status::ErrOverflowOrders;
                } else {
                    ev.l0_write  = true;
                    ev.l0_idx    = req0_idx;
                    ev.l0_qty    = static_cast<uint32_t>(sum);
                    ev.l0_orders = static_cast<uint16_t>(cur_o + 1);
                    ev.l0_active = 1;
                    if (!cur_a) mid_count = static_cast<uint16_t>(mid_count + 1);
                }
                break;
            }
            case Opcode::Cancel: {
                if (!cur_a || req0_qty > cur_q) {
                    ev.status_0 = Status::ErrInvalidCancel;
                } else {
                    ev.l0_write = true;
                    ev.l0_idx   = req0_idx;
                    ev.l0_qty   = cur_q - req0_qty;
                    if (ev.l0_qty == 0) {
                        ev.l0_orders = 0;
                        ev.l0_active = 0;
                        mid_count    = static_cast<uint16_t>(mid_count - 1);
                    } else {
                        ev.l0_orders = (cur_o > 1) ? static_cast<uint16_t>(cur_o - 1) : uint16_t{1};
                        ev.l0_active = 1;
                    }
                }
                break;
            }
            case Opcode::Execute: {
                if (!cur_a || req0_qty > cur_q) {
                    ev.status_0 = Status::ErrInvalidExecute;
                } else {
                    ev.l0_write = true;
                    ev.l0_idx   = req0_idx;
                    ev.l0_qty   = cur_q - req0_qty;
                    if (ev.l0_qty == 0) {
                        ev.l0_orders = 0;
                        ev.l0_active = 0;
                        mid_count    = static_cast<uint16_t>(mid_count - 1);
                    } else {
                        ev.l0_orders = cur_o;
                        ev.l0_active = 1;
                    }
                }
                break;
            }
            default:
                ev.status_0 = Status::ErrUnsupportedOpcode;
                break;
        }
    }

    // Lane 1 evaluation (forwards Lane 0 if same index)
    uint16_t next_count = mid_count;
    if (req1_en) {
        uint32_t cur_q = (ev.l0_write && req1_idx == req0_idx) ? ev.l0_qty    : bank.qty[req1_idx];
        uint16_t cur_o = (ev.l0_write && req1_idx == req0_idx) ? ev.l0_orders : bank.orders[req1_idx];
        uint8_t  cur_a = (ev.l0_write && req1_idx == req0_idx) ? ev.l0_active : bank.active[req1_idx];

        switch (static_cast<Opcode>(req1_op)) {
            case Opcode::Add: {
                uint64_t sum = static_cast<uint64_t>(cur_q) + static_cast<uint64_t>(req1_qty);
                if (!cur_a && mid_count >= cfg_.max_active_levels) {
                    ev.status_1 = Status::ErrFullCapacity;
                } else if (sum > std::numeric_limits<uint32_t>::max()) {
                    ev.status_1 = Status::ErrOverflowQty;
                } else if (cur_o == std::numeric_limits<uint16_t>::max()) {
                    ev.status_1 = Status::ErrOverflowOrders;
                } else {
                    ev.l1_write  = true;
                    ev.l1_idx    = req1_idx;
                    ev.l1_qty    = static_cast<uint32_t>(sum);
                    ev.l1_orders = static_cast<uint16_t>(cur_o + 1);
                    ev.l1_active = 1;
                    if (!cur_a) next_count = static_cast<uint16_t>(mid_count + 1);
                }
                break;
            }
            case Opcode::Cancel: {
                if (!cur_a || req1_qty > cur_q) {
                    ev.status_1 = Status::ErrInvalidCancel;
                } else {
                    ev.l1_write = true;
                    ev.l1_idx   = req1_idx;
                    ev.l1_qty   = cur_q - req1_qty;
                    if (ev.l1_qty == 0) {
                        ev.l1_orders = 0;
                        ev.l1_active = 0;
                        next_count   = static_cast<uint16_t>(mid_count - 1);
                    } else {
                        ev.l1_orders = (cur_o > 1) ? static_cast<uint16_t>(cur_o - 1) : uint16_t{1};
                        ev.l1_active = 1;
                    }
                }
                break;
            }
            case Opcode::Execute: {
                if (!cur_a || req1_qty > cur_q) {
                    ev.status_1 = Status::ErrInvalidExecute;
                } else {
                    ev.l1_write = true;
                    ev.l1_idx   = req1_idx;
                    ev.l1_qty   = cur_q - req1_qty;
                    if (ev.l1_qty == 0) {
                        ev.l1_orders = 0;
                        ev.l1_active = 0;
                        next_count   = static_cast<uint16_t>(mid_count - 1);
                    } else {
                        ev.l1_orders = cur_o;
                        ev.l1_active = 1;
                    }
                }
                break;
            }
            default:
                ev.status_1 = Status::ErrUnsupportedOpcode;
                break;
        }
    }

    ev.next_active_count = next_count;
    return ev;
}

void PipelinedOrderBookAccelerator::commit_bank(BankState& bank, const BankEval& ev) {
    if (ev.l0_write) {
        bank.qty[ev.l0_idx]    = ev.l0_qty;
        bank.orders[ev.l0_idx] = ev.l0_orders;
        bank.active[ev.l0_idx] = ev.l0_active;
    }
    if (ev.l1_write) {
        bank.qty[ev.l1_idx]    = ev.l1_qty;
        bank.orders[ev.l1_idx] = ev.l1_orders;
        bank.active[ev.l1_idx] = ev.l1_active;
    }
    bank.active_count = ev.next_active_count;
}

PipelinedOrderBookAccelerator::TreeResult
PipelinedOrderBookAccelerator::eval_bbo_tree(const std::vector<uint8_t>& active, bool select_highest) {
    // Explicit binary reduction tree mirroring rtl/bbo_tree.sv
    const size_t n = active.size();
    std::vector<TreeResult> layer(n);
    for (size_t i = 0; i < n; ++i) {
        layer[i] = TreeResult{active[i] != 0, static_cast<uint32_t>(i)};
    }
    size_t width = n;
    while (width > 1) {
        const size_t half = width / 2;
        for (size_t i = 0; i < half; ++i) {
            const TreeResult lo = layer[2 * i];
            const TreeResult hi = layer[2 * i + 1];
            TreeResult merged{};
            merged.valid = lo.valid || hi.valid;
            if (select_highest) {
                merged.idx = hi.valid ? hi.idx : lo.idx;
            } else {
                merged.idx = lo.valid ? lo.idx : hi.idx;
            }
            layer[i] = merged;
        }
        width = half;
    }
    return layer[0].valid ? layer[0] : TreeResult{false, 0};
}

TickOutput PipelinedOrderBookAccelerator::tick(const TickInput& in) {
    last_out_ready_ = in.out_ready;

    // Synchronous active-low reset (exact RTL match)
    if (!in.rst_n) {
        uint64_t prev_reset = diag_.reset_cycles;
        hard_reset();
        diag_.reset_cycles = prev_reset + 1;
        return TickOutput{/*in_ready=*/true, /*accepted=*/false, /*out_valid=*/false, BboOutputPacket{}};
    }

    // Combinational pre-clock evaluation (matches SystemVerilog non-blocking semantics)
    const bool pipe_advance = (!out_valid_) || in.out_ready;
    const bool cur_in_ready = pipe_advance;
    const bool accepted     = in.in_valid_0 && cur_in_ready;

    // Stage 3 combinational BBO tree reads pre-edge bank_regs (post-Stage-2 state of s2)
    const TreeResult bid_tree = eval_bbo_tree(bid_bank_.active, /*select_highest=*/true);
    const TreeResult ask_tree = eval_bbo_tree(ask_bank_.active, /*select_highest=*/false);

    BboOutputPacket next_out_bbo{};
    next_out_bbo.schema_ver        = kSchemaVersion;
    next_out_bbo.lane1_valid       = s2_lane1_valid_;
    next_out_bbo.status_0          = s2_status_0_;
    next_out_bbo.status_1          = s2_status_1_;
    next_out_bbo.seq_no_0          = s2_seq_no_0_;
    next_out_bbo.seq_no_1          = s2_seq_no_1_;
    next_out_bbo.timestamp_ns      = s2_timestamp_ns_;
    next_out_bbo.best_bid_valid    = bid_tree.valid;
    next_out_bbo.best_bid_price    = bid_tree.valid ? (cfg_.price_base + bid_tree.idx) : 0;
    next_out_bbo.best_bid_qty      = bid_tree.valid ? bid_bank_.qty[bid_tree.idx] : 0;
    next_out_bbo.best_bid_orders   = bid_tree.valid ? bid_bank_.orders[bid_tree.idx] : 0;
    next_out_bbo.best_ask_valid    = ask_tree.valid;
    next_out_bbo.best_ask_price    = ask_tree.valid ? (cfg_.price_base + ask_tree.idx) : 0;
    next_out_bbo.best_ask_qty      = ask_tree.valid ? ask_bank_.qty[ask_tree.idx] : 0;
    next_out_bbo.best_ask_orders   = ask_tree.valid ? ask_bank_.orders[ask_tree.idx] : 0;
    next_out_bbo.active_bid_levels = bid_bank_.active_count;
    next_out_bbo.active_ask_levels = ask_bank_.active_count;
    if (next_out_bbo.best_bid_valid && next_out_bbo.best_ask_valid) {
        next_out_bbo.spread  = static_cast<int32_t>(next_out_bbo.best_ask_price) -
                               static_cast<int32_t>(next_out_bbo.best_bid_price);
        next_out_bbo.crossed = (next_out_bbo.best_bid_price >= next_out_bbo.best_ask_price);
    }

    // Stage 2 combinational bank evaluation for s1_lane0 / s1_lane1
    const bool bid_req0 = s1_active_ && s1_lane0_.exec_bank &&
                          (s1_lane0_.side == static_cast<uint8_t>(Side::Bid));
    const bool bid_req1 = s1_active_ && s1_lane1_.valid && s1_lane1_.exec_bank &&
                          (s1_lane1_.side == static_cast<uint8_t>(Side::Bid));
    const bool ask_req0 = s1_active_ && s1_lane0_.exec_bank &&
                          (s1_lane0_.side == static_cast<uint8_t>(Side::Ask));
    const bool ask_req1 = s1_active_ && s1_lane1_.valid && s1_lane1_.exec_bank &&
                          (s1_lane1_.side == static_cast<uint8_t>(Side::Ask));

    const BankEval bid_eval = eval_bank(bid_bank_,
                                        bid_req0, s1_lane0_.opcode, s1_lane0_.idx, s1_lane0_.qty,
                                        bid_req1, s1_lane1_.opcode, s1_lane1_.idx, s1_lane1_.qty);
    const BankEval ask_eval = eval_bank(ask_bank_,
                                        ask_req0, s1_lane0_.opcode, s1_lane0_.idx, s1_lane0_.qty,
                                        ask_req1, s1_lane1_.opcode, s1_lane1_.idx, s1_lane1_.qty);

    Status resolved_st0 = Status::Ok;
    if (s1_lane0_.pre_status != Status::Ok)      resolved_st0 = s1_lane0_.pre_status;
    else if (bid_req0)                           resolved_st0 = bid_eval.status_0;
    else if (ask_req0)                           resolved_st0 = ask_eval.status_0;

    Status resolved_st1 = Status::Ok;
    if (!s1_lane1_.valid)                        resolved_st1 = Status::Ok;
    else if (s1_lane1_.pre_status != Status::Ok) resolved_st1 = s1_lane1_.pre_status;
    else if (bid_req1)                           resolved_st1 = bid_eval.status_1;
    else if (ask_req1)                           resolved_st1 = ask_eval.status_1;

    // Stage 1 combinational decode for incoming events
    const DecodedLane next_dec0 = decode_event(in.in_valid_0, in.in_event_0);
    const DecodedLane next_dec1 = decode_event(in.in_valid_0 && in.in_valid_1, in.in_event_1);

    // Rising clock edge register commits
    ++diag_.cycles_total;
    if (in.in_valid_0 && !cur_in_ready) {
        ++diag_.backpressure_stalls;
    }
    if (in.in_valid_0 && cur_in_ready) {
        diag_.events_accepted += (in.in_valid_1 ? 2u : 1u);
        if (in.in_valid_1) {
            ++diag_.dual_lane_cycles;
        }
    }

    if (pipe_advance) {
        if (s2_active_) {
            diag_.record_status(s2_status_0_);
            if (s2_lane1_valid_) {
                diag_.record_status(s2_status_1_);
            }
            diag_.max_active_bid_levels = std::max(diag_.max_active_bid_levels, bid_bank_.active_count);
            diag_.max_active_ask_levels = std::max(diag_.max_active_ask_levels, ask_bank_.active_count);
        }

        // Latch Stage 3 output
        out_valid_ = s2_active_;
        out_bbo_   = next_out_bbo;

        // Commit Stage 2 banks and metadata
        commit_bank(bid_bank_, bid_eval);
        commit_bank(ask_bank_, ask_eval);

        s2_active_       = s1_active_;
        s2_lane1_valid_  = s1_active_ && s1_lane1_.valid;
        s2_status_0_     = resolved_st0;
        s2_status_1_     = resolved_st1;
        s2_seq_no_0_     = s1_lane0_.seq_no;
        s2_seq_no_1_     = s1_lane1_.valid ? s1_lane1_.seq_no : 0;
        s2_timestamp_ns_ = s1_lane1_.valid ? s1_lane1_.timestamp_ns : s1_lane0_.timestamp_ns;

        // Latch Stage 1
        s1_active_ = in.in_valid_0;
        s1_lane0_  = next_dec0;
        s1_lane1_  = next_dec1;
    }

    return TickOutput{(!out_valid_) || in.out_ready, accepted, out_valid_, out_bbo_};
}

// =============================================================================
// BaselineFsmOrderBook (Unpipelined 3-cycle FSM baseline, II = 3)
// =============================================================================

BaselineFsmOrderBook::BaselineFsmOrderBook(Config cfg) : cfg_(cfg) {
    if (auto err = cfg_.validate(); err.has_value()) {
        throw std::invalid_argument("Invalid BaselineFsmOrderBook config: " + *err);
    }
    hard_reset();
}

void BaselineFsmOrderBook::hard_reset() {
    state_   = State::Idle;
    latched_ = {};
    status_  = Status::Ok;
    bid_qty_.assign(cfg_.num_levels, 0);
    bid_ord_.assign(cfg_.num_levels, 0);
    bid_act_.assign(cfg_.num_levels, 0);
    bid_cnt_ = 0;
    ask_qty_.assign(cfg_.num_levels, 0);
    ask_ord_.assign(cfg_.num_levels, 0);
    ask_act_.assign(cfg_.num_levels, 0);
    ask_cnt_   = 0;
    out_valid_ = false;
    out_bbo_   = {};
    diag_      = {};
}

TickOutput BaselineFsmOrderBook::tick(const TickInput& in) {
    if (!in.rst_n) {
        uint64_t prev_reset = diag_.reset_cycles;
        hard_reset();
        diag_.reset_cycles = prev_reset + 1;
        return TickOutput{true, false, false, BboOutputPacket{}};
    }

    ++diag_.cycles_total;
    const bool out_stalled = out_valid_ && !in.out_ready;
    if (out_stalled) {
        if (in.in_valid_0) ++diag_.backpressure_stalls;
        return TickOutput{false, false, out_valid_, out_bbo_};
    }

    out_valid_ = false;
    bool accepted = false;

    switch (state_) {
        case State::Idle: {
            if (in.in_valid_0) {
                latched_ = in.in_event_0;
                state_   = State::Update;
                ++diag_.events_accepted;
                accepted = true;
            }
            break;
        }
        case State::Update: {
            const EventPacket& e = latched_;
            status_ = Status::Ok;
            if (e.schema_ver != kSchemaVersion) status_ = Status::ErrUnsupportedSchema;
            else if (e.flags != 0) status_ = Status::ErrUnsupportedFlags;
            else if (e.opcode > static_cast<uint8_t>(Opcode::Execute)) status_ = Status::ErrUnsupportedOpcode;
            else if (e.opcode != static_cast<uint8_t>(Opcode::Nop)) {
                if (e.side != static_cast<uint8_t>(Side::Bid) && e.side != static_cast<uint8_t>(Side::Ask)) {
                    status_ = Status::ErrInvalidSide;
                } else if (e.price < cfg_.price_base || e.price >= cfg_.price_base + cfg_.num_levels) {
                    status_ = Status::ErrPriceOob;
                } else if (e.qty == 0) {
                    status_ = Status::ErrZeroQty;
                } else {
                    const uint32_t idx = e.price - cfg_.price_base;
                    const bool is_bid  = (e.side == static_cast<uint8_t>(Side::Bid));
                    auto& qty_v = is_bid ? bid_qty_ : ask_qty_;
                    auto& ord_v = is_bid ? bid_ord_ : ask_ord_;
                    auto& act_v = is_bid ? bid_act_ : ask_act_;
                    auto& cnt   = is_bid ? bid_cnt_ : ask_cnt_;

                    if (e.opcode == static_cast<uint8_t>(Opcode::Add)) {
                        uint64_t sum = static_cast<uint64_t>(qty_v[idx]) + e.qty;
                        if (!act_v[idx] && cnt >= cfg_.max_active_levels) status_ = Status::ErrFullCapacity;
                        else if (sum > std::numeric_limits<uint32_t>::max()) status_ = Status::ErrOverflowQty;
                        else if (ord_v[idx] == std::numeric_limits<uint16_t>::max()) status_ = Status::ErrOverflowOrders;
                        else {
                            if (!act_v[idx]) ++cnt;
                            qty_v[idx] = static_cast<uint32_t>(sum);
                            ord_v[idx] = static_cast<uint16_t>(ord_v[idx] + 1);
                            act_v[idx] = 1;
                        }
                    } else if (e.opcode == static_cast<uint8_t>(Opcode::Cancel)) {
                        if (!act_v[idx] || e.qty > qty_v[idx]) status_ = Status::ErrInvalidCancel;
                        else {
                            qty_v[idx] -= e.qty;
                            if (qty_v[idx] == 0) {
                                ord_v[idx] = 0;
                                act_v[idx] = 0;
                                --cnt;
                            } else {
                                ord_v[idx] = (ord_v[idx] > 1) ? static_cast<uint16_t>(ord_v[idx] - 1) : uint16_t{1};
                            }
                        }
                    } else if (e.opcode == static_cast<uint8_t>(Opcode::Execute)) {
                        if (!act_v[idx] || e.qty > qty_v[idx]) status_ = Status::ErrInvalidExecute;
                        else {
                            qty_v[idx] -= e.qty;
                            if (qty_v[idx] == 0) {
                                ord_v[idx] = 0;
                                act_v[idx] = 0;
                                --cnt;
                            }
                        }
                    }
                }
            }
            state_ = State::ScanBbo;
            break;
        }
        case State::ScanBbo: {
            BboOutputPacket out{};
            out.schema_ver        = kSchemaVersion;
            out.status_0          = status_;
            out.seq_no_0          = latched_.seq_no;
            out.timestamp_ns      = latched_.timestamp_ns;
            out.active_bid_levels = bid_cnt_;
            out.active_ask_levels = ask_cnt_;

            for (int i = static_cast<int>(cfg_.num_levels) - 1; i >= 0; --i) {
                if (bid_act_[static_cast<size_t>(i)]) {
                    out.best_bid_valid  = true;
                    out.best_bid_price  = cfg_.price_base + static_cast<uint32_t>(i);
                    out.best_bid_qty    = bid_qty_[static_cast<size_t>(i)];
                    out.best_bid_orders = bid_ord_[static_cast<size_t>(i)];
                    break;
                }
            }
            for (uint32_t i = 0; i < cfg_.num_levels; ++i) {
                if (ask_act_[i]) {
                    out.best_ask_valid  = true;
                    out.best_ask_price  = cfg_.price_base + i;
                    out.best_ask_qty    = ask_qty_[i];
                    out.best_ask_orders = ask_ord_[i];
                    break;
                }
            }
            if (out.best_bid_valid && out.best_ask_valid) {
                out.spread  = static_cast<int32_t>(out.best_ask_price) - static_cast<int32_t>(out.best_bid_price);
                out.crossed = (out.best_bid_price >= out.best_ask_price);
            }
            diag_.record_status(status_);
            diag_.max_active_bid_levels = std::max(diag_.max_active_bid_levels, bid_cnt_);
            diag_.max_active_ask_levels = std::max(diag_.max_active_ask_levels, ask_cnt_);

            out_bbo_   = out;
            out_valid_ = true;
            state_     = State::Idle;
            break;
        }
    }

    return TickOutput{(state_ == State::Idle) && !out_valid_, accepted, out_valid_, out_bbo_};
}

}  // namespace fpga_ob

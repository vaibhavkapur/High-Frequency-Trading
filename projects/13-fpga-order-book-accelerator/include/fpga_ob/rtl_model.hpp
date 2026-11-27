#pragma once

#include <cstdint>
#include <vector>

#include "fpga_ob/contracts.hpp"
#include "fpga_ob/diagnostics.hpp"

namespace fpga_ob {

struct TickInput {
    bool        rst_n      = true;
    bool        in_valid_0 = false;
    EventPacket in_event_0 = {};
    bool        in_valid_1 = false;
    EventPacket in_event_1 = {};
    bool        out_ready  = true;
};

struct TickOutput {
    bool            in_ready  = true;
    bool            accepted  = false;
    bool            out_valid = false;
    BboOutputPacket out_bbo   = {};
};

// Cycle-accurate C++20 mirror of rtl/order_book_top.sv:
// 3-stage pipeline (Decode -> Dual Price-Level Bank Update -> Binary Tree BBO Extract)
// Latency = 3 cycles, II = 1 cycle/event (0.5 cycles/event dual-lane).
class PipelinedOrderBookAccelerator {
public:
    static constexpr uint32_t kPipelineLatencyCycles = 3;

    explicit PipelinedOrderBookAccelerator(Config cfg = Config{});

    void hard_reset();
    TickOutput tick(const TickInput& in);

    [[nodiscard]] bool in_ready() const noexcept { return !out_valid_ || last_out_ready_; }
    [[nodiscard]] bool out_valid() const noexcept { return out_valid_; }
    [[nodiscard]] const BboOutputPacket& out_bbo() const noexcept { return out_bbo_; }
    [[nodiscard]] const Diagnostics& diagnostics() const noexcept { return diag_; }
    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

private:
    struct DecodedLane {
        bool     valid        = false;
        bool     exec_bank    = false;
        Status   pre_status   = Status::Ok;
        uint8_t  opcode       = 0;
        uint8_t  side         = 0;
        uint32_t idx          = 0;
        uint32_t qty          = 0;
        uint32_t seq_no       = 0;
        uint32_t timestamp_ns = 0;
    };

    struct BankState {
        std::vector<uint32_t> qty;
        std::vector<uint16_t> orders;
        std::vector<uint8_t>  active;
        uint16_t              active_count = 0;

        void init(uint32_t n) {
            qty.assign(n, 0);
            orders.assign(n, 0);
            active.assign(n, 0);
            active_count = 0;
        }
    };

    struct BankEval {
        Status   status_0          = Status::Ok;
        bool     l0_write          = false;
        uint32_t l0_idx            = 0;
        uint32_t l0_qty            = 0;
        uint16_t l0_orders         = 0;
        uint8_t  l0_active         = 0;

        Status   status_1          = Status::Ok;
        bool     l1_write          = false;
        uint32_t l1_idx            = 0;
        uint32_t l1_qty            = 0;
        uint16_t l1_orders         = 0;
        uint8_t  l1_active         = 0;

        uint16_t next_active_count = 0;
    };

    struct TreeResult {
        bool     valid = false;
        uint32_t idx   = 0;
    };

    DecodedLane decode_event(bool valid, const EventPacket& pkt) const;
    BankEval eval_bank(const BankState& bank,
                       bool req0_en, uint8_t req0_op, uint32_t req0_idx, uint32_t req0_qty,
                       bool req1_en, uint8_t req1_op, uint32_t req1_idx, uint32_t req1_qty) const;
    static void commit_bank(BankState& bank, const BankEval& ev);
    static TreeResult eval_bbo_tree(const std::vector<uint8_t>& active, bool select_highest);

    Config cfg_;

    // Stage 1 registers
    bool        s1_active_ = false;
    DecodedLane s1_lane0_  = {};
    DecodedLane s1_lane1_  = {};

    // Stage 2 registers (price-level banks + metadata)
    BankState bid_bank_;
    BankState ask_bank_;
    bool      s2_active_       = false;
    bool      s2_lane1_valid_  = false;
    Status    s2_status_0_     = Status::Ok;
    Status    s2_status_1_     = Status::Ok;
    uint32_t  s2_seq_no_0_     = 0;
    uint32_t  s2_seq_no_1_     = 0;
    uint32_t  s2_timestamp_ns_ = 0;

    // Stage 3 output registers
    bool            out_valid_      = false;
    BboOutputPacket out_bbo_        = {};
    bool            last_out_ready_ = true;

    Diagnostics diag_ = {};
};

// Non-pipelined 3-state FSM baseline (II = 3 cycles/event, single lane)
// for like-for-like cycle-latency & throughput benchmarking against the
// pipelined hardware architecture.
class BaselineFsmOrderBook {
public:
    static constexpr uint32_t kCyclesPerEvent = 3;

    explicit BaselineFsmOrderBook(Config cfg = Config{});

    void hard_reset();
    TickOutput tick(const TickInput& in);

    [[nodiscard]] const Diagnostics& diagnostics() const noexcept { return diag_; }

private:
    enum class State : uint8_t { Idle, Update, ScanBbo };

    Config                cfg_;
    State                 state_   = State::Idle;
    EventPacket           latched_ = {};
    Status                status_  = Status::Ok;
    std::vector<uint32_t> bid_qty_;
    std::vector<uint16_t> bid_ord_;
    std::vector<uint8_t>  bid_act_;
    uint16_t              bid_cnt_ = 0;
    std::vector<uint32_t> ask_qty_;
    std::vector<uint16_t> ask_ord_;
    std::vector<uint8_t>  ask_act_;
    uint16_t              ask_cnt_ = 0;

    bool            out_valid_ = false;
    BboOutputPacket out_bbo_   = {};
    Diagnostics     diag_      = {};
};

}  // namespace fpga_ob

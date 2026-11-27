#pragma once

#include <cstdint>
#include <map>

#include "fpga_ob/contracts.hpp"

namespace fpga_ob {

// Independent software golden reference model using std::map (no bitmasks,
// no binary reduction tree, no pipelining). Used as the oracle for all
// deterministic, integration, and randomized differential tests.
class ReferenceOrderBook {
public:
    explicit ReferenceOrderBook(Config cfg = Config{});

    void reset();

    [[nodiscard]] BboOutputPacket apply_single(const EventPacket& e);
    [[nodiscard]] BboOutputPacket apply_pair(const EventPacket& e0, const EventPacket& e1);
    [[nodiscard]] BboOutputPacket snapshot(uint32_t seq0, uint32_t seq1, bool lane1_valid,
                                           Status st0, Status st1, uint32_t ts_ns) const;

    [[nodiscard]] const Config& config() const noexcept { return cfg_; }
    [[nodiscard]] size_t active_bid_levels() const noexcept { return bids_.size(); }
    [[nodiscard]] size_t active_ask_levels() const noexcept { return asks_.size(); }

private:
    struct Level {
        uint32_t qty    = 0;
        uint16_t orders = 0;
    };

    Status apply_one(const EventPacket& e);

    Config                      cfg_;
    std::map<uint32_t, Level>   bids_;
    std::map<uint32_t, Level>   asks_;
};

}  // namespace fpga_ob

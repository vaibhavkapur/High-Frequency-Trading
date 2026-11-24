// Latency model: three independent paths plus strategy compute time.
#pragma once

#include <algorithm>

#include "hftbt/rng.hpp"
#include "hftbt/types.hpp"

namespace hftbt {

struct LatencyConfig {
    Timestamp md_delay_ns = 0;      // exchange -> strategy market data
    Timestamp md_jitter_ns = 0;     // uniform [0, jitter] added per event
    Timestamp compute_delay_ns = 0; // strategy callback -> order leaves the box
    Timestamp order_delay_ns = 0;   // strategy -> exchange order entry
    Timestamp order_jitter_ns = 0;
    Timestamp response_delay_ns = 0; // exchange -> strategy exec reports
    Timestamp response_jitter_ns = 0;
};

// One direction of one link. Delivery times never go backwards: each path
// models an ordered session (TCP-like), so jitter cannot reorder messages.
class LatencyPath {
public:
    LatencyPath(Timestamp base, Timestamp jitter, std::uint64_t seed)
        : base_(std::max<Timestamp>(0, base)), jitter_(std::max<Timestamp>(0, jitter)), rng_(seed) {
    }

    Timestamp deliver_at(Timestamp sent) noexcept {
        Timestamp t = sent + base_;
        if (jitter_ > 0) t += rng_.range(0, jitter_);
        last_ = std::max(last_, t);
        return last_;
    }

private:
    Timestamp base_;
    Timestamp jitter_;
    Timestamp last_ = 0;
    Rng rng_;
};

} // namespace hftbt

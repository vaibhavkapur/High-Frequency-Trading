#include "obce/fixture_generator.hpp"

#include <algorithm>
#include <array>
#include <climits>

namespace obce {

namespace {

// Platform-independent SplitMix64 + Xoshiro256** PRNG so identical seeds produce
// byte-identical synthetic streams across macOS, Linux, and Windows.
struct DeterministicRng {
    std::array<uint64_t, 4> s{};

    explicit DeterministicRng(uint64_t seed) {
        uint64_t z = seed;
        for (size_t i = 0; i < 4; ++i) {
            z += 0x9E3779B97F4A7C15ULL;
            uint64_t x = z;
            x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
            x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
            s[i] = x ^ (x >> 31U);
        }
    }

    static uint64_t rotl(uint64_t x, int k) noexcept {
        return (x << k) | (x >> (64 - k));
    }

    uint64_t next_u64() noexcept {
        const uint64_t result = rotl(s[1] * 5ULL, 7) * 9ULL;
        const uint64_t t = s[1] << 17U;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return result;
    }

    uint64_t uniform(uint64_t bound) noexcept {
        return bound == 0 ? 0 : (next_u64() % bound);
    }

    int64_t signed_range(int64_t lo, int64_t hi) noexcept {
        const uint64_t span = static_cast<uint64_t>(hi - lo) + 1ULL;
        return lo + static_cast<int64_t>(uniform(span));
    }
};

}  // namespace

const char* workload_profile_name(WorkloadProfile profile) noexcept {
    switch (profile) {
        case WorkloadProfile::L3OrderFlow: return "l3_order_flow";
        case WorkloadProfile::L2TopOfBook: return "l2_top_of_book";
        case WorkloadProfile::VolatileBursty: return "volatile_bursty";
        case WorkloadProfile::ExtremeDeltas: return "extreme_deltas";
    }
    return "unknown";
}

std::vector<MarketEvent> generate_synthetic_events(const GeneratorConfig& cfg) {
    DeterministicRng rng(cfg.seed);
    std::vector<MarketEvent> events;
    events.reserve(cfg.event_count);

    const uint32_t num_syms = std::max<uint32_t>(1U, cfg.num_symbols);
    std::vector<int64_t> mid_prices(num_syms);
    for (uint32_t i = 0; i < num_syms; ++i) {
        // Base synthetic prices around $100.00 .. $450.00 in 1e-8 tick scale
        mid_prices[i] = (100LL + static_cast<int64_t>(i) * 50LL) * kDefaultPriceScale;
    }

    uint64_t ts = cfg.start_timestamp_ns;
    uint64_t seq = cfg.start_seq_id;
    uint64_t next_order_id = 10'000'000ULL;
    uint64_t next_match_id = 500'000ULL;
    uint32_t active_sym_idx = 0;

    for (size_t i = 0; i < cfg.event_count; ++i) {
        MarketEvent ev{};
        ev.seq_id = seq++;

        if (cfg.profile == WorkloadProfile::L3OrderFlow) {
            // Burst locality on symbol (80% same symbol as previous event)
            if (rng.uniform(100) >= 80) {
                active_sym_idx = static_cast<uint32_t>(rng.uniform(num_syms));
            }
            ts += 250ULL + rng.uniform(5'000ULL);  // 250ns..5.25us inter-arrival
            ev.timestamp_ns = ts;
            ev.symbol_id = active_sym_idx + 1U;
            ev.scope = BookScope::L3;
            ev.side = (rng.uniform(2) == 0) ? Side::Bid : Side::Ask;

            const uint64_t roll = rng.uniform(100);
            if (roll < 42) {
                ev.type = EventType::AddOrder;
                ev.order_id = ++next_order_id;
                ev.quantity = rng.signed_range(1, 50) * 100LL;
            } else if (roll < 74) {
                ev.type = EventType::CancelOrder;
                const uint64_t lookback = 1ULL + rng.uniform(std::min<uint64_t>(64ULL, next_order_id - 9'999'999ULL));
                ev.order_id = next_order_id - lookback + 1ULL;
                ev.quantity = 0;
            } else if (roll < 90) {
                ev.type = EventType::ModifyOrder;
                const uint64_t lookback = 1ULL + rng.uniform(std::min<uint64_t>(32ULL, next_order_id - 9'999'999ULL));
                ev.order_id = next_order_id - lookback + 1ULL;
                ev.quantity = rng.signed_range(1, 40) * 100LL;
            } else {
                ev.type = EventType::ExecuteOrder;
                const uint64_t lookback = 1ULL + rng.uniform(std::min<uint64_t>(16ULL, next_order_id - 9'999'999ULL));
                ev.order_id = next_order_id - lookback + 1ULL;
                ev.quantity = rng.signed_range(1, 20) * 100LL;
                if (cfg.schema_version == kSchemaVersionV2) {
                    ev.match_id = ++next_match_id;
                }
            }

            // Small random walk in tick size ($0.01 = 1,000,000 in 1e-8 units)
            const int64_t tick = 1'000'000LL;
            mid_prices[active_sym_idx] += rng.signed_range(-2, 2) * tick;
            const int64_t offset_ticks = (ev.side == Side::Bid ? -1 : 1) * rng.signed_range(0, 5);
            ev.price_ticks = mid_prices[active_sym_idx] + offset_ticks * tick;
            ev.level_index = 0;

            if (cfg.schema_version == kSchemaVersionV2) {
                ev.venue_id = 1;
                ev.flags = (ev.type == EventType::ExecuteOrder) ? 0x0002U : 0x0001U;
            }
        } else if (cfg.profile == WorkloadProfile::L2TopOfBook) {
            if (rng.uniform(100) >= 85) {
                active_sym_idx = static_cast<uint32_t>(rng.uniform(num_syms));
            }
            ts += 100ULL + rng.uniform(2'000ULL);
            ev.timestamp_ns = ts;
            ev.symbol_id = active_sym_idx + 1U;
            ev.scope = BookScope::L2;
            ev.type = (i % 500 == 0) ? EventType::L2Snapshot : EventType::L2Update;
            ev.side = (rng.uniform(2) == 0) ? Side::Bid : Side::Ask;
            ev.order_id = 0;
            ev.level_index = static_cast<uint16_t>(rng.uniform(10));
            const int64_t tick = 1'000'000LL;
            mid_prices[active_sym_idx] += rng.signed_range(-1, 1) * tick;
            ev.price_ticks = mid_prices[active_sym_idx] +
                             (ev.side == Side::Bid ? -1 : 1) * (static_cast<int64_t>(ev.level_index) + 1LL) * tick;
            ev.quantity = rng.signed_range(0, 500) * 100LL;
            if (cfg.schema_version == kSchemaVersionV2) {
                ev.venue_id = 2;
                ev.flags = (ev.type == EventType::L2Snapshot) ? 0x0010U : 0x0000U;
                ev.match_id = 0;
            }
        } else if (cfg.profile == WorkloadProfile::VolatileBursty) {
            active_sym_idx = static_cast<uint32_t>(rng.uniform(num_syms));
            ts += rng.uniform(250'000ULL);
            ev.timestamp_ns = ts;
            ev.symbol_id = active_sym_idx + 1U;
            ev.scope = BookScope::L3;
            ev.side = (rng.uniform(2) == 0) ? Side::Bid : Side::Ask;
            ev.type = static_cast<EventType>(1U + rng.uniform(4));
            ev.order_id = 1ULL + rng.next_u64() % 1'000'000'000ULL;
            ev.price_ticks = mid_prices[active_sym_idx] + rng.signed_range(-5000, 5000) * 100'000LL;
            ev.quantity = rng.signed_range(0, 100'000);
            if (cfg.schema_version == kSchemaVersionV2) {
                ev.venue_id = static_cast<uint16_t>(1U + rng.uniform(4));
                ev.flags = static_cast<uint16_t>(rng.uniform(16));
                ev.match_id = (ev.type == EventType::ExecuteOrder) ? (++next_match_id) : 0ULL;
            }
        } else {
            // ExtremeDeltas: intentional signed/unsigned boundary swings (negative prices, INT64_MAX/MIN, huge order_ids)
            ts += (i % 2 == 0) ? 1ULL : 10'000'000'000ULL;
            ev.timestamp_ns = ts;
            ev.symbol_id = (i % 2 == 0) ? 1U : UINT32_MAX;
            ev.scope = BookScope::L3;
            ev.side = (i % 2 == 0) ? Side::Bid : Side::Ask;
            ev.type = (i % 3 == 0) ? EventType::AddOrder : EventType::ModifyOrder;
            ev.order_id = (i % 2 == 0) ? 1ULL : UINT64_MAX;
            ev.price_ticks = (i % 2 == 0) ? INT64_MIN : INT64_MAX;
            ev.quantity = (i % 2 == 0) ? 0LL : INT64_MAX;
            ev.level_index = 0;
            if (cfg.schema_version == kSchemaVersionV2) {
                ev.venue_id = (i % 2 == 0) ? 0U : UINT16_MAX;
                ev.flags = (i % 2 == 0) ? 0U : UINT16_MAX;
                ev.match_id = (i % 2 == 0) ? 0ULL : UINT64_MAX;
            }
        }

        events.push_back(ev);
    }

    return events;
}

}  // namespace obce

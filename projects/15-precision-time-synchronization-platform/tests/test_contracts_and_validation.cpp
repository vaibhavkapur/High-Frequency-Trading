#include "ptsp/io.hpp"
#include "ptsp/types.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " — " << (msg) << "\n";      \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

namespace {

ptsp::TimestampedPacket make_valid_packet() {
    ptsp::TimestampedPacket p{};
    p.packet_id = 1;
    p.sequence_id = 1;
    p.source_id = "feed-a";
    p.symbol = "SYNTH-EQ1";
    p.event_ts.value_ns = 1'700'000'000'000'000'000LL;
    p.event_ts.uncertainty_ns = 25;
    p.event_ts.provenance = {
        ptsp::ClockDomain::Tai,
        ptsp::TimestampRole::EventTime,
        ptsp::TimestampCaptureMethod::SyntheticFixture,
        ptsp::ErrorSource::SyntheticFixture,
        "venue-a",
        0
    };
    p.receive_ts.value_ns = 1'700'000'000'000'002'000LL;
    p.receive_ts.uncertainty_ns = 30;
    p.receive_ts.provenance = {
        ptsp::ClockDomain::MonotonicRaw,
        ptsp::TimestampRole::ReceiveTime,
        ptsp::TimestampCaptureMethod::SyntheticFixture,
        ptsp::ErrorSource::SyntheticFixture,
        "local-osc-0",
        0
    };
    return p;
}

}  // namespace

int main() {
    // 1) Valid packet contract passes.
    {
        auto pkt = make_valid_packet();
        auto res = ptsp::validate_packet(pkt);
        CHECK(res.ok(), "Valid packet should pass validation");
    }

    // 2) Swapped event-time vs receive-time roles MUST be rejected.
    {
        auto pkt = make_valid_packet();
        pkt.event_ts.provenance.role = ptsp::TimestampRole::ReceiveTime;
        auto res = ptsp::validate_packet(pkt);
        CHECK(!res.ok() && res.code == ptsp::ErrorCode::InvalidTimestamp,
              "event_ts with RECEIVE_TIME role must be rejected");
    }

    // 3) Negative uncertainty or non-positive timestamp rejected.
    {
        auto pkt = make_valid_packet();
        pkt.receive_ts.uncertainty_ns = -5;
        auto res = ptsp::validate_packet(pkt);
        CHECK(!res.ok() && res.code == ptsp::ErrorCode::InvalidTimestamp,
              "Negative uncertainty must be rejected");

        pkt.receive_ts.uncertainty_ns = 0;
        pkt.event_ts.value_ns = 0;
        res = ptsp::validate_packet(pkt);
        CHECK(!res.ok() && res.code == ptsp::ErrorCode::InvalidTimestamp,
              "Non-positive timestamp value must be rejected");
    }

    // 4) Strict domain guard never silently mixes domains.
    {
        ptsp::ClockProvenance tai{ptsp::ClockDomain::Tai, ptsp::TimestampRole::EventTime,
                                  ptsp::TimestampCaptureMethod::SyntheticFixture,
                                  ptsp::ErrorSource::SyntheticFixture, "gm-tai", 0};
        ptsp::ClockProvenance utc{ptsp::ClockDomain::Utc, ptsp::TimestampRole::EventTime,
                                  ptsp::TimestampCaptureMethod::SyntheticFixture,
                                  ptsp::ErrorSource::SyntheticFixture, "ntp-utc", 0};
        auto res = ptsp::require_same_domain(tai, utc, "unit-test");
        CHECK(!res.ok() && res.code == ptsp::ErrorCode::ClockDomainMismatch,
              "TAI vs UTC must trigger ClockDomainMismatch");
    }

    // 5) Invalid sync sample fixture (negative RTT) rejected by validate_sync_sample.
    {
        const std::filesystem::path fix_dir = std::filesystem::path(PTSP_SOURCE_DIR) / "fixtures";
        std::vector<ptsp::SyncExchangeSample> bad_samples;
        auto load_res = ptsp::load_sync_samples_csv((fix_dir / "invalid_negative_rtt_sync.csv").string(), bad_samples);
        CHECK(load_res.ok() && bad_samples.size() == 1, "Should parse raw CSV row before domain validation");
        auto v_res = ptsp::validate_sync_sample(bad_samples[0]);
        CHECK(!v_res.ok() && v_res.code == ptsp::ErrorCode::InvalidSyncSample,
              "Negative RTT sample must fail validation");
    }

    // 6) Boundary fixture (0 ns uncertainty, 0 ns transit) is valid.
    {
        const std::filesystem::path fix_dir = std::filesystem::path(PTSP_SOURCE_DIR) / "fixtures";
        std::vector<ptsp::TimestampedPacket> pkts;
        auto load_res = ptsp::load_packets_csv((fix_dir / "boundary_zero_uncertainty_packets.csv").string(), pkts);
        CHECK(load_res.ok() && pkts.size() == 1, "Boundary fixture must load");
        auto v_res = ptsp::validate_packet(pkts[0]);
        CHECK(v_res.ok(), "Zero uncertainty boundary packet should validate");
    }

    // 7) Missing config, schema mismatch, and unknown config key fail explicitly (no silent success).
    {
        const std::filesystem::path fix_dir = std::filesystem::path(PTSP_SOURCE_DIR) / "fixtures";
        ptsp::ScenarioConfig cfg{};

        auto r_missing = ptsp::load_scenario_config_kv((fix_dir / "does_not_exist.cfg").string(), cfg);
        CHECK(!r_missing.ok() && r_missing.code == ptsp::ErrorCode::MissingConfig,
              "Missing config must return MissingConfig");

        auto r_schema = ptsp::load_scenario_config_kv((fix_dir / "invalid_schema.cfg").string(), cfg);
        CHECK(!r_schema.ok() && r_schema.code == ptsp::ErrorCode::SchemaVersionMismatch,
              "Bad schema_version must return SchemaVersionMismatch");

        auto r_key = ptsp::load_scenario_config_kv((fix_dir / "invalid_unknown_key.cfg").string(), cfg);
        CHECK(!r_key.ok() && r_key.code == ptsp::ErrorCode::InvalidConfig,
              "Unknown key must return InvalidConfig");
    }

    std::cout << "[PASS] test_contracts_and_validation\n";
    return 0;
}

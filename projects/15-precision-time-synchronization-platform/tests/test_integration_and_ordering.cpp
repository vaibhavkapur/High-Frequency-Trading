#include "ptsp/fail_closed_orderer.hpp"
#include "ptsp/io.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " — " << (msg) << "\n";      \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

namespace {

ptsp::ClockEstimate make_healthy_estimate(ptsp::Nanos anchor_ns, ptsp::Nanos offset_ns, ptsp::Nanos unc_ns = 60) {
    ptsp::ClockEstimate est{};
    est.valid = true;
    est.seq = 10;
    est.estimator_epoch = 0;
    est.reference_domain = ptsp::ClockDomain::Tai;
    est.local_domain = ptsp::ClockDomain::MonotonicRaw;
    est.reference_clock_id = "gm-tai-0";
    est.local_clock_id = "local-osc-0";
    est.error_source = ptsp::ErrorSource::EstimatedModel;
    est.input_error_source = ptsp::ErrorSource::SyntheticFixture;
    est.anchor_ref_ns = anchor_ns;
    est.offset_ns = offset_ns;
    est.drift_ppb = 0.0;
    est.total_uncertainty_ns = unc_ns;
    est.drift_uncertainty_ppb = 5.0;
    return est;
}

ptsp::TimestampedPacket make_pkt(
    std::uint64_t id,
    std::string source,
    ptsp::Nanos event_ns,
    ptsp::Nanos event_unc_ns,
    ptsp::Nanos rx_ns,
    ptsp::ClockDomain ev_dom = ptsp::ClockDomain::Tai,
    ptsp::ClockDomain rx_dom = ptsp::ClockDomain::MonotonicRaw
) {
    ptsp::TimestampedPacket p{};
    p.packet_id = id;
    p.sequence_id = id;
    p.source_id = std::move(source);
    p.symbol = "SYNTH-EQ1";
    p.event_ts = {
        event_ns, event_unc_ns,
        {ev_dom, ptsp::TimestampRole::EventTime, ptsp::TimestampCaptureMethod::SyntheticFixture,
         ptsp::ErrorSource::SyntheticFixture, "venue-clk", 0}
    };
    p.receive_ts = {
        rx_ns, 20,
        {rx_dom, ptsp::TimestampRole::ReceiveTime, ptsp::TimestampCaptureMethod::SyntheticFixture,
         ptsp::ErrorSource::SyntheticFixture, "local-osc-0", 0}
    };
    return p;
}

}  // namespace

int main() {
    const std::filesystem::path fix_dir = std::filesystem::path(PTSP_SOURCE_DIR) / "fixtures";

    // 1) Out-of-order integration fixture: reordered deterministically into monotonic event-time order.
    {
        ptsp::ScenarioConfig cfg{};
        CHECK(ptsp::load_scenario_config_kv((fix_dir / "out_of_order_stream.cfg").string(), cfg).ok(),
              "load out_of_order_stream.cfg");
        std::vector<ptsp::SyncExchangeSample> sync_samples;
        std::vector<ptsp::TimestampedPacket> packets;
        CHECK(ptsp::load_sync_samples_csv((fix_dir / cfg.sync_fixture_path).string(), sync_samples).ok(),
              "load ooo sync");
        CHECK(ptsp::load_packets_csv((fix_dir / cfg.packet_fixture_path).string(), packets).ok(),
              "load ooo pkts");

        ptsp::ScenarioEvaluationSummary sum1{};
        ptsp::ScenarioEvaluationSummary sum2{};
        CHECK(ptsp::run_scenario(cfg, sync_samples, packets, sum1).ok(), "run ooo 1");
        CHECK(ptsp::run_scenario(cfg, sync_samples, packets, sum2).ok(), "run ooo 2");

        // Reproducibility check: identical stable fingerprint across repeated runs.
        CHECK(sum1.run_fingerprint == sum2.run_fingerprint, "Repeated runs must produce identical run_fingerprint");
        CHECK(sum1.final_orderer_state == ptsp::OrderingState::Healthy, "OOO within window remains Healthy");
        CHECK(sum1.emitted_events.size() == packets.size(), "All 16 OOO packets emitted after flush");
        CHECK(sum1.orderer_counters.packets_reordered == 4, "Reordered counter must record all 4 out-of-order pair inversions");

        for (std::size_t i = 1; i < sum1.emitted_events.size(); ++i) {
            CHECK(sum1.emitted_events[i].packet.event_ts.value_ns >=
                      sum1.emitted_events[i - 1].packet.event_ts.value_ns,
                  "Emitted events must be strictly non-decreasing in event_time");
        }
    }

    // 2) Skew violation fail-closed fixture: halts immediately on causal skew breach and rejects subsequent packets.
    {
        ptsp::ScenarioConfig cfg{};
        CHECK(ptsp::load_scenario_config_kv((fix_dir / "skew_violation_fail_closed.cfg").string(), cfg).ok(),
              "load skew_violation_fail_closed.cfg");
        std::vector<ptsp::SyncExchangeSample> sync_samples;
        std::vector<ptsp::TimestampedPacket> packets;
        CHECK(ptsp::load_sync_samples_csv((fix_dir / cfg.sync_fixture_path).string(), sync_samples).ok(),
              "load skew sync");
        CHECK(ptsp::load_packets_csv((fix_dir / cfg.packet_fixture_path).string(), packets).ok(),
              "load skew pkts");

        ptsp::ScenarioEvaluationSummary sum{};
        CHECK(ptsp::run_scenario(cfg, sync_samples, packets, sum).ok(), "run skew scenario");
        CHECK(sum.final_orderer_state == ptsp::OrderingState::HaltedFailClosed,
              "Orderer must transition to HALTED_FAIL_CLOSED on causal skew violation");
        CHECK(sum.orderer_halt_code == ptsp::ErrorCode::CausalViolation,
              "Halt code must be CausalViolation");
        CHECK(sum.orderer_counters.causal_violations == 1, "Exactly 1 causal violation recorded");
        CHECK(sum.orderer_counters.rejected_while_halted == 4,
              "All 4 packets arriving after halt must be rejected fail-closed");
    }

    // 3) Cross-domain mixing in orderer MUST be rejected (never silently mix TAI and UTC).
    {
        ptsp::OrdererConfig cfg{};
        ptsp::FailClosedOrderer orderer(cfg);
        const ptsp::Nanos t0 = 1'700'000'000'000'000'000LL;
        const auto est = make_healthy_estimate(t0, 5'000);
        std::vector<ptsp::OrderedEvent> out;

        auto p_ok = make_pkt(1, "feed-a", t0 + 1'000, 20, t0 + 1'000 + 5'000 + 1'000, ptsp::ClockDomain::Tai);
        auto p_utc = make_pkt(2, "feed-b", t0 + 5'000, 20, t0 + 5'000 + 5'000 + 1'000, ptsp::ClockDomain::Utc);

        CHECK(orderer.ingest(p_ok, est, out).ok(), "TAI packet accepted");
        auto r_bad = orderer.ingest(p_utc, est, out);
        CHECK(!r_bad.ok() && r_bad.code == ptsp::ErrorCode::ClockDomainMismatch,
              "UTC event packet must be rejected when orderer expects TAI");
        CHECK(orderer.counters().domain_mismatches == 1, "domain_mismatches counter incremented");
    }

    // 4) Cross-source overlapping uncertainty intervals trigger fail-closed AmbiguousOrdering,
    //    and operator recovery restores Healthy state.
    {
        ptsp::OrdererConfig cfg{};
        cfg.reorder_window_ns = 1'000;
        cfg.fail_closed_on_ambiguity = true;
        ptsp::FailClosedOrderer orderer(cfg);
        const ptsp::Nanos t0 = 1'700'000'000'000'000'000LL;
        const auto est = make_healthy_estimate(t0, 5'000);
        std::vector<ptsp::OrderedEvent> out;

        // feed-a at t0+1000 +/- 150 overlaps feed-b at t0+1100 +/- 150!
        auto p1 = make_pkt(1, "feed-a", t0 + 1'000, 150, t0 + 1'000 + 5'000 + 1'000);
        auto p2 = make_pkt(2, "feed-b", t0 + 1'100, 150, t0 + 1'100 + 5'000 + 1'000);
        auto p3 = make_pkt(3, "feed-a", t0 + 5'000, 20, t0 + 5'000 + 5'000 + 1'000);

        CHECK(orderer.ingest(p1, est, out).ok(), "buffer p1");
        CHECK(orderer.ingest(p2, est, out).ok(), "buffer p2");
        auto r3 = orderer.ingest(p3, est, out);
        CHECK(!r3.ok() && r3.code == ptsp::ErrorCode::AmbiguousOrdering,
              "Overlapping cross-source uncertainty intervals must halt fail-closed");
        CHECK(orderer.state() == ptsp::OrderingState::HaltedFailClosed, "State is HaltedFailClosed");
        CHECK(orderer.flush(out).code == ptsp::ErrorCode::FailClosedHalted,
              "Flush must be refused while halted fail-closed");

        // Operator recovery resets buffer and returns to Healthy.
        orderer.recover_and_reset_buffer("verified-skew-resolved");
        CHECK(orderer.state() == ptsp::OrderingState::Healthy, "Recovered state is Healthy");
        CHECK(orderer.counters().manual_recoveries == 1, "manual_recoveries incremented");
    }

    // 5) Bounded capacity enforcement (never silently drops or allocates unboundedly).
    {
        ptsp::OrdererConfig cfg{};
        cfg.max_buffered_events = 2;
        cfg.reorder_window_ns = 100'000;
        ptsp::FailClosedOrderer orderer(cfg);
        const ptsp::Nanos t0 = 1'700'000'000'000'000'000LL;
        const auto est = make_healthy_estimate(t0, 0);
        std::vector<ptsp::OrderedEvent> out;

        CHECK(orderer.ingest(make_pkt(1, "feed-a", t0 + 1'000, 10, t0 + 2'000), est, out).ok(), "slot 1");
        CHECK(orderer.ingest(make_pkt(2, "feed-a", t0 + 2'000, 10, t0 + 3'000), est, out).ok(), "slot 2");
        auto r3 = orderer.ingest(make_pkt(3, "feed-a", t0 + 3'000, 10, t0 + 4'000), est, out);
        CHECK(!r3.ok() && r3.code == ptsp::ErrorCode::BufferCapacityExceeded,
              "Exceeding max_buffered_events must return BufferCapacityExceeded");
        CHECK(orderer.counters().buffer_overflows == 1, "buffer_overflows incremented");
    }

    std::cout << "[PASS] test_integration_and_ordering\n";
    return 0;
}

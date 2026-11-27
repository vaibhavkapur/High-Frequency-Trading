#include "ptsp/adapters.hpp"
#include "ptsp/io.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " — " << (msg) << "\n";      \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

int main() {
    // 1) SoftwareTimestampAdapter: real OS probe reports MEASURED provenance and refuses TAI.
    {
        ptsp::SoftwareTimestampAdapter sw("test-sw-clock", 100);
        const auto rep = sw.probe_capability();
        CHECK(rep.available, "POSIX software clock should be available on host");
        CHECK(!rep.hardware_timestamping_supported, "Software adapter must never claim HW timestamping");
        CHECK(rep.error_source == ptsp::ErrorSource::Measured,
              "Software probe must report ErrorSource::Measured");
        CHECK(rep.clock_resolution_ns >= 1, "Clock resolution must be positive");
        CHECK(rep.probe_samples == 100, "Probe samples recorded");

        ptsp::ProvenancedTimestamp ts{};
        auto ok_mono = sw.capture_now(ptsp::ClockDomain::Monotonic, ptsp::TimestampRole::ReceiveTime, ts);
        CHECK(ok_mono.ok(), "Software adapter captures MONOTONIC");
        CHECK(ts.provenance.error_source == ptsp::ErrorSource::Measured,
              "Captured software timestamp must be tagged MEASURED");
        CHECK(ts.provenance.domain == ptsp::ClockDomain::Monotonic, "Domain preserved");
        CHECK(ts.value_ns > 0 && ts.uncertainty_ns >= 1, "Positive timestamp and uncertainty");

        // Software clock without verified PTP cannot fabricate TAI timestamps.
        auto bad_tai = sw.capture_now(ptsp::ClockDomain::Tai, ptsp::TimestampRole::EventTime, ts);
        CHECK(!bad_tai.ok() && bad_tai.code == ptsp::ErrorCode::UnsupportedClockDomain,
              "Software adapter must refuse unverified TAI domain");
    }

    // 2) Unavailable hardware timestamp capability MUST fail explicitly and never fabricate timestamps.
    {
        // Point to a non-existent PHC device / interface (and on macOS, Linux PTP is natively unavailable).
        ptsp::LinuxPtpHardwareAdapter hw("nonexistent_nic0", "/dev/ptp_nonexistent_99");
        const auto rep = hw.probe_capability();
        CHECK(!rep.available, "Unavailable HW timestamp adapter must report available=false");
        CHECK(!rep.hardware_timestamping_supported, "hardware_timestamping_supported must be false");
        CHECK(rep.status_code == ptsp::ErrorCode::HardwareTimestampUnavailable,
              "Must return ErrorCode::HardwareTimestampUnavailable");
        CHECK(rep.error_source == ptsp::ErrorSource::Measured,
              "Capability probe itself reflects real host state (MEASURED)");

        ptsp::ProvenancedTimestamp ts{};
        auto cap = hw.capture_now(ptsp::ClockDomain::Tai, ptsp::TimestampRole::ReceiveTime, ts);
        CHECK(!cap.ok() && cap.code == ptsp::ErrorCode::HardwareTimestampUnavailable,
              "capture_now must fail explicitly with HardwareTimestampUnavailable");
    }

    // 3) SyntheticFixtureTimestampAdapter: explicitly tags SYNTHETIC_FIXTURE and enforces domain match.
    {
        ptsp::ProvenancedTimestamp sample{};
        sample.value_ns = 1'700'000'000'000'000'000LL;
        sample.uncertainty_ns = 15;
        sample.provenance = {
            ptsp::ClockDomain::Tai,
            ptsp::TimestampRole::EventTime,
            ptsp::TimestampCaptureMethod::SyntheticFixture,
            ptsp::ErrorSource::SyntheticFixture,
            "synth-gm-0",
            0
        };
        ptsp::SyntheticFixtureTimestampAdapter synth({sample});
        const auto rep = synth.probe_capability();
        CHECK(rep.available && !rep.hardware_timestamping_supported, "Synthetic adapter ready, not HW");
        CHECK(rep.error_source == ptsp::ErrorSource::SyntheticFixture,
              "Synthetic adapter must report ErrorSource::SyntheticFixture");

        ptsp::ProvenancedTimestamp out{};
        auto mismatch = synth.capture_now(ptsp::ClockDomain::Utc, ptsp::TimestampRole::EventTime, out);
        CHECK(!mismatch.ok() && mismatch.code == ptsp::ErrorCode::ClockDomainMismatch,
              "Synthetic adapter must reject mismatched requested domain");

        auto ok = synth.capture_now(ptsp::ClockDomain::Tai, ptsp::TimestampRole::EventTime, out);
        CHECK(ok.ok() && out.provenance.error_source == ptsp::ErrorSource::SyntheticFixture,
              "Synthetic capture succeeds with SYNTHETIC_FIXTURE provenance");
    }

    std::cout << "[PASS] test_adapters_and_hw_failure\n";
    return 0;
}

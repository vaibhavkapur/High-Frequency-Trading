#pragma once

#include "ptsp/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ptsp {

struct AdapterCapabilityReport {
    std::string adapter_name{};
    std::string platform{};
    TimestampCaptureMethod capture_method{TimestampCaptureMethod::Unknown};
    ErrorSource error_source{ErrorSource::Measured};
    bool available{false};
    bool hardware_timestamping_supported{false};
    ErrorCode status_code{ErrorCode::Ok};
    std::string status_message{};

    // Measured diagnostics (populated only when available == true and error_source == Measured).
    Nanos clock_resolution_ns{0};
    Nanos median_read_latency_ns{0};
    Nanos p99_read_latency_ns{0};
    Nanos measured_realtime_vs_monotonic_jitter_ns{0};
    std::size_t probe_samples{0};
};

class ITimestampAdapter {
public:
    virtual ~ITimestampAdapter() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual AdapterCapabilityReport probe_capability() = 0;
    [[nodiscard]] virtual ValidationResult capture_now(
        ClockDomain requested_domain,
        TimestampRole role,
        ProvenancedTimestamp& out_ts
    ) = 0;
};

// Real OS software clock adapter (works on Linux and macOS).
// Measures real clock resolution and call-overhead uncertainty via clock_gettime().
class SoftwareTimestampAdapter final : public ITimestampAdapter {
public:
    explicit SoftwareTimestampAdapter(std::string clock_id = "host-sw-clock", std::size_t probe_iterations = 200);

    [[nodiscard]] std::string name() const override { return "software_clock_adapter"; }
    [[nodiscard]] AdapterCapabilityReport probe_capability() override;
    [[nodiscard]] ValidationResult capture_now(
        ClockDomain requested_domain,
        TimestampRole role,
        ProvenancedTimestamp& out_ts
    ) override;

private:
    std::string clock_id_;
    std::size_t probe_iterations_;
    bool probed_{false};
    Nanos measured_uncertainty_ns_{500};
    AdapterCapabilityReport cached_report_{};
};

// Linux PTP / NIC hardware timestamping adapter (SO_TIMESTAMPING / /dev/ptp*).
// Explicitly fails closed with ErrorCode::HardwareTimestampUnavailable when the host OS,
// NIC driver, or PHC device cannot provide verified hardware timestamps.
class LinuxPtpHardwareAdapter final : public ITimestampAdapter {
public:
    explicit LinuxPtpHardwareAdapter(
        std::string interface_name = "eth0",
        std::string ptp_device_path = "/dev/ptp0"
    );

    [[nodiscard]] std::string name() const override { return "linux_ptp_hw_adapter"; }
    [[nodiscard]] AdapterCapabilityReport probe_capability() override;
    [[nodiscard]] ValidationResult capture_now(
        ClockDomain requested_domain,
        TimestampRole role,
        ProvenancedTimestamp& out_ts
    ) override;

private:
    std::string interface_name_;
    std::string ptp_device_path_;
};

// Offline deterministic adapter backed by versioned synthetic fixtures.
// Always tags provenance with TimestampCaptureMethod::SyntheticFixture and ErrorSource::SyntheticFixture.
class SyntheticFixtureTimestampAdapter final : public ITimestampAdapter {
public:
    explicit SyntheticFixtureTimestampAdapter(
        std::vector<ProvenancedTimestamp> schedule,
        std::string clock_id = "synthetic-fixture-clock"
    );

    [[nodiscard]] std::string name() const override { return "synthetic_fixture_adapter"; }
    [[nodiscard]] AdapterCapabilityReport probe_capability() override;
    [[nodiscard]] ValidationResult capture_now(
        ClockDomain requested_domain,
        TimestampRole role,
        ProvenancedTimestamp& out_ts
    ) override;

    void reset() noexcept { cursor_ = 0; }

private:
    std::vector<ProvenancedTimestamp> schedule_;
    std::string clock_id_;
    std::size_t cursor_{0};
};

}  // namespace ptsp

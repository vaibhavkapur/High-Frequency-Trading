#include "ptsp/adapters.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <sstream>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(PTSP_HAVE_LINUX_NET_TSTAMP_H)
#include <linux/net_tstamp.h>
#endif
#if defined(PTSP_HAVE_LINUX_PTP_CLOCK_H)
#include <linux/ptp_clock.h>
#endif
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#endif

namespace ptsp {

namespace {

std::string host_platform_name() {
#if defined(__APPLE__)
    return "Darwin (macOS)";
#elif defined(__linux__)
    return "Linux";
#else
    return "Other";
#endif
}

Nanos timespec_to_ns(const struct timespec& ts) noexcept {
    return static_cast<Nanos>(ts.tv_sec) * 1'000'000'000LL + static_cast<Nanos>(ts.tv_nsec);
}

bool read_posix_clock(ClockDomain domain, Nanos& out_ns) noexcept {
    struct timespec ts{};
    clockid_t cid = CLOCK_MONOTONIC;
    switch (domain) {
        case ClockDomain::Utc:
            cid = CLOCK_REALTIME;
            break;
        case ClockDomain::Monotonic:
            cid = CLOCK_MONOTONIC;
            break;
        case ClockDomain::MonotonicRaw:
#if defined(CLOCK_MONOTONIC_RAW)
            cid = CLOCK_MONOTONIC_RAW;
#else
            cid = CLOCK_MONOTONIC;
#endif
            break;
        default:
            return false;
    }
    if (clock_gettime(cid, &ts) != 0) {
        return false;
    }
    out_ns = timespec_to_ns(ts);
    return true;
}

}  // namespace

// ---------------- SoftwareTimestampAdapter ----------------

SoftwareTimestampAdapter::SoftwareTimestampAdapter(std::string clock_id, std::size_t probe_iterations)
    : clock_id_(std::move(clock_id)),
      probe_iterations_(std::max<std::size_t>(probe_iterations, 20)) {}

AdapterCapabilityReport SoftwareTimestampAdapter::probe_capability() {
    AdapterCapabilityReport rep{};
    rep.adapter_name = name();
    rep.platform = host_platform_name();
    rep.capture_method = TimestampCaptureMethod::UserspaceClock;
    rep.error_source = ErrorSource::Measured;
    rep.hardware_timestamping_supported = false;

    struct timespec res{};
    if (clock_getres(CLOCK_MONOTONIC, &res) != 0) {
        rep.available = false;
        rep.status_code = ErrorCode::UnsupportedPlatform;
        rep.status_message = "clock_getres(CLOCK_MONOTONIC) failed";
        cached_report_ = rep;
        probed_ = true;
        return rep;
    }

    rep.clock_resolution_ns = std::max<Nanos>(1, timespec_to_ns(res));

    std::vector<Nanos> read_latencies;
    read_latencies.reserve(probe_iterations_);
    std::vector<Nanos> rt_minus_mono;
    rt_minus_mono.reserve(probe_iterations_);

    for (std::size_t i = 0; i < probe_iterations_; ++i) {
        Nanos t_a = 0;
        Nanos t_rt = 0;
        Nanos t_b = 0;
        if (!read_posix_clock(ClockDomain::Monotonic, t_a) ||
            !read_posix_clock(ClockDomain::Utc, t_rt) ||
            !read_posix_clock(ClockDomain::Monotonic, t_b)) {
            rep.available = false;
            rep.status_code = ErrorCode::IoError;
            rep.status_message = "clock_gettime failed during software clock probe";
            cached_report_ = rep;
            probed_ = true;
            return rep;
        }
        const Nanos dt = std::max<Nanos>(0, t_b - t_a);
        read_latencies.push_back(dt);
        const Nanos mid_mono = t_a + (dt / 2);
        rt_minus_mono.push_back(t_rt - mid_mono);
    }

    std::sort(read_latencies.begin(), read_latencies.end());
    std::sort(rt_minus_mono.begin(), rt_minus_mono.end());

    const std::size_t med_idx = read_latencies.size() / 2;
    const std::size_t p99_idx = std::min(read_latencies.size() - 1, (read_latencies.size() * 99) / 100);

    rep.available = true;
    rep.status_code = ErrorCode::Ok;
    rep.status_message = "POSIX software clock available (measured userspace read overhead; not NIC HW PTP)";
    rep.median_read_latency_ns = read_latencies[med_idx];
    rep.p99_read_latency_ns = read_latencies[p99_idx];
    rep.measured_realtime_vs_monotonic_jitter_ns =
        (rt_minus_mono.back() - rt_minus_mono.front()) / 2;
    rep.probe_samples = probe_iterations_;

    measured_uncertainty_ns_ =
        std::max<Nanos>(rep.clock_resolution_ns, rep.p99_read_latency_ns / 2 + rep.measured_realtime_vs_monotonic_jitter_ns);
    cached_report_ = rep;
    probed_ = true;
    return rep;
}

ValidationResult SoftwareTimestampAdapter::capture_now(
    ClockDomain requested_domain,
    TimestampRole role,
    ProvenancedTimestamp& out_ts
) {
    if (!probed_) {
        (void)probe_capability();
    }
    if (!cached_report_.available) {
        return ValidationResult::fail(cached_report_.status_code, cached_report_.status_message);
    }

    // Explicitly refuse TAI or ExchangeVenue: software OS clocks without verified PTP TAI offset cannot supply them.
    if (requested_domain != ClockDomain::Utc &&
        requested_domain != ClockDomain::Monotonic &&
        requested_domain != ClockDomain::MonotonicRaw) {
        std::ostringstream oss;
        oss << "SoftwareTimestampAdapter cannot natively provide clock domain "
            << to_string(requested_domain) << " without a verified PTP synchronization source";
        return ValidationResult::fail(ErrorCode::UnsupportedClockDomain, oss.str());
    }

    Nanos now_ns = 0;
    if (!read_posix_clock(requested_domain, now_ns)) {
        return ValidationResult::fail(ErrorCode::IoError, "clock_gettime failed");
    }

    out_ts.value_ns = now_ns;
    out_ts.uncertainty_ns = measured_uncertainty_ns_;
    out_ts.provenance.domain = requested_domain;
    out_ts.provenance.role = role;
    out_ts.provenance.capture_method = TimestampCaptureMethod::UserspaceClock;
    out_ts.provenance.error_source = ErrorSource::Measured;
    out_ts.provenance.clock_id = clock_id_;
    out_ts.provenance.epoch_id = 0;
    return ValidationResult::success();
}

// ---------------- LinuxPtpHardwareAdapter ----------------

LinuxPtpHardwareAdapter::LinuxPtpHardwareAdapter(
    std::string interface_name,
    std::string ptp_device_path
)
    : interface_name_(std::move(interface_name)),
      ptp_device_path_(std::move(ptp_device_path)) {}

AdapterCapabilityReport LinuxPtpHardwareAdapter::probe_capability() {
    AdapterCapabilityReport rep{};
    rep.adapter_name = name();
    rep.platform = host_platform_name();
    rep.capture_method = TimestampCaptureMethod::HardwareNicPtp;
    rep.error_source = ErrorSource::Measured;
    rep.available = false;
    rep.hardware_timestamping_supported = false;

#if !defined(__linux__)
    rep.status_code = ErrorCode::HardwareTimestampUnavailable;
    rep.status_message =
        "Linux PTP / SO_TIMESTAMPING NIC hardware timestamping is unavailable on platform " +
        host_platform_name() + " (refusing to fabricate hardware timestamps)";
    return rep;
#else
    int fd = ::open(ptp_device_path_.c_str(), O_RDONLY);
    if (fd < 0) {
        std::ostringstream oss;
        oss << "PTP hardware clock device " << ptp_device_path_
            << " is unavailable (" << std::strerror(errno)
            << "); hardware timestamping capability not present or unprivileged";
        rep.status_code = ErrorCode::HardwareTimestampUnavailable;
        rep.status_message = oss.str();
        return rep;
    }

    // Verify ethtool timestamping info on requested interface.
    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ::close(fd);
        rep.status_code = ErrorCode::HardwareTimestampUnavailable;
        rep.status_message = "Unable to open control socket for ETHTOOL_GET_TS_INFO probe";
        return rep;
    }

    struct ifreq ifr{};
    struct ethtool_ts_info ts_info{};
    ts_info.cmd = ETHTOOL_GET_TS_INFO;
    std::strncpy(ifr.ifr_name, interface_name_.c_str(), IFNAMSIZ - 1);
    ifr.ifr_data = reinterpret_cast<char*>(&ts_info);

    bool hw_ok = false;
    if (::ioctl(sock, SIOCETHTOOL, &ifr) == 0) {
#if defined(SOF_TIMESTAMPING_TX_HARDWARE) && defined(SOF_TIMESTAMPING_RX_HARDWARE) && defined(SOF_TIMESTAMPING_RAW_HARDWARE)
        const std::uint32_t required =
            SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
        hw_ok = ((ts_info.so_timestamping & required) == required) && (ts_info.phc_index >= 0);
#endif
    }
    ::close(sock);
    ::close(fd);

    if (!hw_ok) {
        rep.status_code = ErrorCode::HardwareTimestampUnavailable;
        rep.status_message = "Interface " + interface_name_ +
                             " does not report TX/RX RAW_HARDWARE PTP timestamping with an active PHC";
        return rep;
    }

    rep.available = true;
    rep.hardware_timestamping_supported = true;
    rep.status_code = ErrorCode::Ok;
    rep.status_message = "Linux PHC and NIC hardware timestamping reported available on " + interface_name_;
    return rep;
#endif
}

ValidationResult LinuxPtpHardwareAdapter::capture_now(
    ClockDomain requested_domain,
    TimestampRole role,
    ProvenancedTimestamp& out_ts
) {
    auto rep = probe_capability();
    if (!rep.available || !rep.hardware_timestamping_supported) {
        return ValidationResult::fail(ErrorCode::HardwareTimestampUnavailable, rep.status_message);
    }

#if defined(__linux__)
    if (requested_domain != ClockDomain::Tai) {
        return ValidationResult::fail(
            ErrorCode::UnsupportedClockDomain,
            "LinuxPtpHardwareAdapter PHC operates in TAI domain only"
        );
    }
    int fd = ::open(ptp_device_path_.c_str(), O_RDONLY);
    if (fd < 0) {
        return ValidationResult::fail(
            ErrorCode::HardwareTimestampUnavailable,
            "Failed to open PHC device " + ptp_device_path_
        );
    }
    // Dynamic POSIX clock ID derived from PHC file descriptor (Linux convention: (~fd << 3) | 3).
    const clockid_t phc_cid = static_cast<clockid_t>((~static_cast<unsigned int>(fd) << 3U) | 3U);
    struct timespec ts{};
    struct timespec res{};
    const int rc = clock_gettime(phc_cid, &ts);
    const int rrc = clock_getres(phc_cid, &res);
    ::close(fd);
    if (rc != 0) {
        return ValidationResult::fail(
            ErrorCode::HardwareTimestampUnavailable,
            "clock_gettime on Linux PHC failed"
        );
    }
    out_ts.value_ns = timespec_to_ns(ts);
    out_ts.uncertainty_ns = (rrc == 0) ? std::max<Nanos>(1, timespec_to_ns(res)) : 50;
    out_ts.provenance.domain = ClockDomain::Tai;
    out_ts.provenance.role = role;
    out_ts.provenance.capture_method = TimestampCaptureMethod::HardwareNicPtp;
    out_ts.provenance.error_source = ErrorSource::Measured;
    out_ts.provenance.clock_id = "phc:" + interface_name_;
    out_ts.provenance.epoch_id = 0;
    return ValidationResult::success();
#else
    (void)requested_domain;
    (void)role;
    (void)out_ts;
    return ValidationResult::fail(ErrorCode::HardwareTimestampUnavailable, rep.status_message);
#endif
}

// ---------------- SyntheticFixtureTimestampAdapter ----------------

SyntheticFixtureTimestampAdapter::SyntheticFixtureTimestampAdapter(
    std::vector<ProvenancedTimestamp> schedule,
    std::string clock_id
)
    : schedule_(std::move(schedule)), clock_id_(std::move(clock_id)) {}

AdapterCapabilityReport SyntheticFixtureTimestampAdapter::probe_capability() {
    AdapterCapabilityReport rep{};
    rep.adapter_name = name();
    rep.platform = host_platform_name();
    rep.capture_method = TimestampCaptureMethod::SyntheticFixture;
    rep.error_source = ErrorSource::SyntheticFixture;
    rep.available = !schedule_.empty();
    rep.hardware_timestamping_supported = false;
    rep.status_code = rep.available ? ErrorCode::Ok : ErrorCode::InvalidConfig;
    rep.status_message = rep.available
        ? "Offline deterministic synthetic fixture adapter ready (not measured hardware PTP)"
        : "Synthetic fixture schedule is empty";
    rep.probe_samples = schedule_.size();
    return rep;
}

ValidationResult SyntheticFixtureTimestampAdapter::capture_now(
    ClockDomain requested_domain,
    TimestampRole role,
    ProvenancedTimestamp& out_ts
) {
    if (cursor_ >= schedule_.size()) {
        return ValidationResult::fail(ErrorCode::IoError, "SyntheticFixtureTimestampAdapter schedule exhausted");
    }
    const auto& next = schedule_[cursor_];
    if (next.provenance.domain != requested_domain) {
        std::ostringstream oss;
        oss << "SyntheticFixtureTimestampAdapter domain mismatch: requested "
            << to_string(requested_domain) << " but fixture carries "
            << to_string(next.provenance.domain);
        return ValidationResult::fail(ErrorCode::ClockDomainMismatch, oss.str());
    }
    if (next.provenance.role != role) {
        return ValidationResult::fail(
            ErrorCode::InvalidTimestamp,
            "SyntheticFixtureTimestampAdapter role mismatch"
        );
    }
    out_ts = next;
    out_ts.provenance.capture_method = TimestampCaptureMethod::SyntheticFixture;
    out_ts.provenance.error_source = ErrorSource::SyntheticFixture;
    if (out_ts.provenance.clock_id.empty()) {
        out_ts.provenance.clock_id = clock_id_;
    }
    ++cursor_;
    return ValidationResult::success();
}

}  // namespace ptsp

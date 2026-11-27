#include "ptsp/estimator.hpp"
#include "ptsp/fail_closed_orderer.hpp"
#include "ptsp/io.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct BenchStats {
    std::string name{};
    std::size_t warmup_ops{0};
    std::size_t measured_ops{0};
    double mean_ns{0.0};
    std::int64_t p50_ns{0};
    std::int64_t p95_ns{0};
    std::int64_t p99_ns{0};
    std::int64_t max_ns{0};
    double throughput_mops{0.0};
};

BenchStats summarize(const std::string& name, std::size_t warmup, std::vector<std::int64_t>& samples) {
    BenchStats st{};
    st.name = name;
    st.warmup_ops = warmup;
    st.measured_ops = samples.size();
    if (samples.empty()) return st;

    double sum = 0.0;
    for (auto v : samples) sum += static_cast<double>(v);
    st.mean_ns = sum / static_cast<double>(samples.size());

    std::sort(samples.begin(), samples.end());
    auto pct = [&](double p) -> std::int64_t {
        const std::size_t idx = std::min(samples.size() - 1, static_cast<std::size_t>(p * static_cast<double>(samples.size())));
        return samples[idx];
    };
    st.p50_ns = pct(0.50);
    st.p95_ns = pct(0.95);
    st.p99_ns = pct(0.99);
    st.max_ns = samples.back();
    st.throughput_mops = (st.mean_ns > 0.0) ? (1000.0 / st.mean_ns) : 0.0;
    return st;
}

std::string compiler_info() {
#if defined(__clang__)
    std::ostringstream oss;
    oss << "Clang " << __clang_major__ << "." << __clang_minor__ << "." << __clang_patchlevel__;
    return oss.str();
#elif defined(__GNUC__)
    std::ostringstream oss;
    oss << "GCC " << __GNUC__ << "." << __GNUC_MINOR__ << "." << __GNUC_PATCHLEVEL__;
    return oss.str();
#else
    return "Unknown";
#endif
}

std::string build_mode() {
#if defined(NDEBUG)
    return "Release (-O3 -DNDEBUG)";
#else
    return "Debug/RelWithDebInfo";
#endif
}

std::string platform_arch() {
#if defined(__APPLE__) && defined(__aarch64__)
    return "Darwin arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "Darwin x86_64";
#elif defined(__linux__) && defined(__x86_64__)
    return "Linux x86_64";
#elif defined(__linux__) && defined(__aarch64__)
    return "Linux aarch64";
#else
    return "Other";
#endif
}

std::vector<ptsp::SyncExchangeSample> build_deterministic_stream(std::size_t count) {
    std::vector<ptsp::SyncExchangeSample> stream;
    stream.reserve(count);
    std::uint64_t state = 0x123456789ABCDEF0ULL;
    auto next_noise = [&]() -> ptsp::Nanos {
        // Deterministic xorshift64*
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const std::uint64_t r = (state * 2685821657736338717ULL) >> 56;
        return static_cast<ptsp::Nanos>(r % 25) - 12;
    };

    const ptsp::Nanos t0 = 1'700'000'000'000'000'000LL;
    for (std::size_t i = 0; i < count; ++i) {
        ptsp::SyncExchangeSample s{};
        s.seq = i + 1;
        s.epoch_id = 0;
        s.reference_domain = ptsp::ClockDomain::Tai;
        s.local_domain = ptsp::ClockDomain::MonotonicRaw;
        s.reference_clock_id = "gm-tai-0";
        s.local_clock_id = "local-osc-0";
        s.capture_method = ptsp::TimestampCaptureMethod::SyntheticFixture;
        s.error_source = ptsp::ErrorSource::SyntheticFixture;

        const ptsp::Nanos t1 = t0 + static_cast<ptsp::Nanos>(i) * 100'000'000LL;  // 10 Hz sync
        const ptsp::Nanos offset = 10'000 + static_cast<ptsp::Nanos>(i * 15);
        ptsp::Nanos df = 800 + next_noise();
        ptsp::Nanos dr = 800 + next_noise();
        if (i > 100 && (i % 97 == 0)) {
            df += 15'000;  // Periodic asymmetric spike
        }
        s.t1_ns = t1;
        s.t2_ns = t1 + df + offset;
        s.t3_ns = s.t2_ns + 1'000;
        s.t4_ns = s.t3_ns - offset + dr;
        stream.push_back(s);
    }
    return stream;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_path;
    std::size_t warmup = 2'000;
    std::size_t measured = 20'000;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--measured" && i + 1 < argc) {
            measured = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (arg == "--warmup" && i + 1 < argc) {
            warmup = static_cast<std::size_t>(std::stoull(argv[++i]));
        }
    }

    const std::size_t total = warmup + measured;
    const auto stream = build_deterministic_stream(total);

    ptsp::EstimatorConfig est_cfg{};
    est_cfg.window_size = 32;

    // 1) BaselineOlsEstimator on identical stream
    std::vector<std::int64_t> base_lat;
    base_lat.reserve(measured);
    ptsp::BaselineOlsEstimator baseline(est_cfg);
    ptsp::ClockEstimate est{};
    volatile std::int64_t sink = 0;

    for (std::size_t i = 0; i < total; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        (void)baseline.update(stream[i], est);
        const auto t1 = std::chrono::steady_clock::now();
        sink += est.offset_ns;
        if (i >= warmup) {
            base_lat.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }
    }
    const auto st_base = summarize("baseline_ols_estimator_update", warmup, base_lat);

    // 2) RobustSyncEstimator on identical stream
    std::vector<std::int64_t> rob_lat;
    rob_lat.reserve(measured);
    ptsp::RobustSyncEstimator robust(est_cfg);
    ptsp::ClockEstimate last_robust_est{};

    for (std::size_t i = 0; i < total; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        (void)robust.update(stream[i], last_robust_est);
        const auto t1 = std::chrono::steady_clock::now();
        sink += last_robust_est.offset_ns;
        if (i >= warmup) {
            rob_lat.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }
    }
    const auto st_rob = summarize("robust_sync_estimator_update", warmup, rob_lat);

    // 3) FailClosedOrderer ingest+release on identical packet count
    std::vector<std::int64_t> ord_lat;
    ord_lat.reserve(measured);
    ptsp::OrdererConfig ord_cfg{};
    ord_cfg.max_buffered_events = 256;
    ord_cfg.reorder_window_ns = 1'000;
    ord_cfg.max_skew_ns = 25'000;
    ptsp::FailClosedOrderer orderer(ord_cfg);

    ptsp::ClockEstimate fixed_est = last_robust_est;
    fixed_est.total_uncertainty_ns = 80;
    std::vector<ptsp::OrderedEvent> released;
    released.reserve(16);

    const ptsp::Nanos base_ev = fixed_est.anchor_ref_ns;
    for (std::size_t i = 0; i < total; ++i) {
        ptsp::TimestampedPacket p{};
        p.packet_id = i + 1;
        p.sequence_id = i + 1;
        p.source_id = "feed-a";
        p.symbol = "SYNTH-EQ1";
        p.event_ts = {
            base_ev + static_cast<ptsp::Nanos>(i) * 500, 15,
            {ptsp::ClockDomain::Tai, ptsp::TimestampRole::EventTime,
             ptsp::TimestampCaptureMethod::SyntheticFixture, ptsp::ErrorSource::SyntheticFixture, "venue-a", 0}
        };
        p.receive_ts = {
            p.event_ts.value_ns + fixed_est.offset_ns + 1'200, 15,
            {ptsp::ClockDomain::MonotonicRaw, ptsp::TimestampRole::ReceiveTime,
             ptsp::TimestampCaptureMethod::SyntheticFixture, ptsp::ErrorSource::SyntheticFixture, "local-osc-0", 0}
        };
        released.clear();
        const auto t0 = std::chrono::steady_clock::now();
        (void)orderer.ingest(p, fixed_est, released);
        const auto t1 = std::chrono::steady_clock::now();
        sink += static_cast<std::int64_t>(released.size());
        if (i >= warmup) {
            ord_lat.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }
    }
    const auto st_ord = summarize("fail_closed_orderer_ingest", warmup, ord_lat);
    (void)sink;

    auto emit_bench = [](const BenchStats& b) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "    {\n"
            << "      \"benchmark\": \"" << b.name << "\",\n"
            << "      \"warmup_ops\": " << b.warmup_ops << ",\n"
            << "      \"measured_ops\": " << b.measured_ops << ",\n"
            << "      \"mean_ns\": " << b.mean_ns << ",\n"
            << "      \"p50_ns\": " << b.p50_ns << ",\n"
            << "      \"p95_ns\": " << b.p95_ns << ",\n"
            << "      \"p99_ns\": " << b.p99_ns << ",\n"
            << "      \"max_ns\": " << b.max_ns << ",\n"
            << "      \"throughput_mops\": " << b.throughput_mops << "\n"
            << "    }";
        return oss.str();
    };

    std::ostringstream json;
    json << "{\n"
         << "  \"schema_version\": \"" << ptsp::kSchemaVersion << "\",\n"
         << "  \"latency_error_source\": \"MEASURED\",\n"
         << "  \"input_stream_source\": \"SYNTHETIC_FIXTURE\",\n"
         << "  \"clock_source\": \"std::chrono::steady_clock (CLOCK_MONOTONIC)\",\n"
         << "  \"platform\": \"" << platform_arch() << "\",\n"
         << "  \"compiler\": \"" << compiler_info() << "\",\n"
         << "  \"build_mode\": \"" << build_mode() << "\",\n"
         << "  \"window_size\": " << est_cfg.window_size << ",\n"
         << "  \"benchmarks\": [\n"
         << emit_bench(st_base) << ",\n"
         << emit_bench(st_rob) << ",\n"
         << emit_bench(st_ord) << "\n"
         << "  ]\n"
         << "}\n";

    std::cout << json.str();
    if (!out_path.empty()) {
        auto wr = ptsp::write_text_file(out_path, json.str());
        if (!wr.ok()) {
            std::cerr << "Error writing benchmark output: " << wr.message << "\n";
            return 1;
        }
    }
    return 0;
}

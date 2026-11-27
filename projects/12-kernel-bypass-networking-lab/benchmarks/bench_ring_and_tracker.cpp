#include "kbnl/clock.hpp"
#include "kbnl/json_writer.hpp"
#include "kbnl/latency_stats.hpp"
#include "kbnl/protocol.hpp"
#include "kbnl/ring_buffer.hpp"
#include "kbnl/sequence_tracker.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Pure domain hot-path microbenchmark (in-memory decode + SPSC ring + SequenceTracker + LatencyStats).
// Explicitly labeled as DOMAIN_HOTPATH_MICROBENCH (NOT a hardware NIC / kernel-bypass measurement).
int main(int argc, char** argv) {
    uint64_t iterations = 200000;
    std::string output_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--iterations" && i + 1 < argc) {
            iterations = std::stoull(argv[++i]);
        } else if (arg == "--output" && i + 1 < argc) {
            output_path = argv[++i];
        }
    }

    constexpr size_t kPktBytes = 64;
    constexpr uint64_t kSeed = 20261127ULL;
    constexpr uint64_t kWarmup = 10000;

    // Pre-encode deterministic packet template set (1024 packets cycled) so encode cost is isolated from decode/track path.
    std::vector<std::array<uint8_t, kPktBytes>> prebuilt(1024);
    for (size_t i = 0; i < prebuilt.size(); ++i) {
        kbnl::encode_deterministic_packet(i + 1, 1000, 1, kbnl::kFlagNone, kSeed, kPktBytes, prebuilt[i]);
    }

    struct Slot {
        uint64_t recv_ts_ns;
        uint32_t len;
        std::array<uint8_t, kPktBytes> data;
    };

    kbnl::SpscRingBuffer<Slot> ring(1024);
    kbnl::SequenceTracker tracker;
    kbnl::LatencyStats stats;
    const auto clock_meta = kbnl::get_clock_metadata();

    std::array<uint8_t, kPktBytes> pkt_buf{};
    uint64_t start_ns = 0;
    for (uint64_t i = 0; i < kWarmup + iterations; ++i) {
        const bool is_warmup = i < kWarmup;
        if (i == kWarmup) {
            start_ns = kbnl::now_ns();
        }
        const uint64_t seq = i + 1;
        const uint16_t flags = is_warmup ? kbnl::kFlagWarmup : kbnl::kFlagNone;
        kbnl::encode_deterministic_packet(seq, 0, 1, flags, kSeed, kPktBytes, pkt_buf);

        const uint64_t t0 = kbnl::now_ns();
        Slot s{};
        s.recv_ts_ns = t0;
        s.len = kPktBytes;
        s.data = pkt_buf;
        (void)ring.try_push(s);

        Slot out{};
        if (ring.try_pop(out)) {
            kbnl::DecodedPacket decoded{};
            const auto err = kbnl::decode_packet(std::span<const uint8_t>(out.data.data(), out.len), decoded);
            if (err == kbnl::DecodeError::kOk) {
                (void)tracker.on_packet(decoded.header);
                const uint64_t t1 = kbnl::now_ns();
                stats.record_ns(t1 >= t0 ? (t1 - t0) : 0, is_warmup);
            } else {
                tracker.on_decode_error(err);
            }
        }
    }
    const uint64_t end_ns = kbnl::now_ns();
    const uint64_t dur_ns = end_ns > start_ns ? (end_ns - start_ns) : 1;
    const double ops_per_sec = static_cast<double>(iterations) * 1e9 / static_cast<double>(dur_ns);
    const auto lat = stats.summarize();
    const auto& c = tracker.counters();

    std::ostringstream os;
    os << std::fixed << std::setprecision(3);
    os << "{\n"
       << "  \"schema_version\": 1,\n"
       << "  \"benchmark_type\": \"DOMAIN_HOTPATH_MICROBENCH\",\n"
       << "  \"execution_status\": \"MEASURED\",\n"
       << "  \"is_simulated\": false,\n"
       << "  \"disclaimer\": \"Measures pure in-memory SPSC ring + CRC32 decode + SequenceTracker + LatencyStats overhead; NOT a network or NIC bypass benchmark.\",\n"
       << "  \"timestamp_utc\": \"" << kbnl::json_escape(kbnl::utc_timestamp_iso8601()) << "\",\n"
       << "  \"clock\": {\n"
       << "    \"name\": \"" << kbnl::json_escape(clock_meta.name) << "\",\n"
       << "    \"measured_call_overhead_ns\": " << clock_meta.measured_call_overhead_ns << "\n"
       << "  },\n"
       << "  \"iterations\": " << iterations << ",\n"
       << "  \"warmup_iterations\": " << kWarmup << ",\n"
       << "  \"packet_bytes\": " << kPktBytes << ",\n"
       << "  \"duration_ns\": " << dur_ns << ",\n"
       << "  \"throughput_ops_per_sec\": " << ops_per_sec << ",\n"
       << "  \"accounting_invariant_holds\": " << (c.accounting_invariant_holds() ? "true" : "false") << ",\n"
       << "  \"unique_received\": " << c.unique_received << ",\n"
       << "  \"decode_errors\": " << c.decode_errors << ",\n"
       << "  \"ring_drops\": " << ring.dropped_count() << ",\n"
       << "  \"processing_latency_ns\": {\n"
       << "    \"min\": " << lat.min_ns << ",\n"
       << "    \"mean\": " << lat.mean_ns << ",\n"
       << "    \"p50\": " << lat.p50_ns << ",\n"
       << "    \"p95\": " << lat.p95_ns << ",\n"
       << "    \"p99\": " << lat.p99_ns << ",\n"
       << "    \"p99_9\": " << lat.p999_ns << ",\n"
       << "    \"max\": " << lat.max_ns << "\n"
       << "  }\n"
       << "}\n";

    if (!output_path.empty()) {
        std::string err;
        if (!kbnl::write_text_file(output_path, os.str(), err)) {
            std::cerr << err << "\n";
            return 1;
        }
    } else {
        std::cout << os.str();
    }
    return c.accounting_invariant_holds() ? 0 : 1;
}

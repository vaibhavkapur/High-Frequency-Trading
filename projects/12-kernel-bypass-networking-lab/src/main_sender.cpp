#include "kbnl/clock.hpp"
#include "kbnl/config.hpp"
#include "kbnl/cpu_affinity.hpp"
#include "kbnl/pacer.hpp"
#include "kbnl/posix_udp_transport.hpp"
#include "kbnl/protocol.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

void print_usage() {
    std::cerr << "Usage: kbnl_sender [options]\n"
              << "  --target-ip <ip>       Destination IPv4 (default: 127.0.0.1)\n"
              << "  --port <port>          Destination UDP port (required, 1..65535)\n"
              << "  --pps <rate>           Target packets per second (default: 10000)\n"
              << "  --warmup <n>           Warmup packets (default: 1000)\n"
              << "  --count <n>            Measured packets (default: 10000)\n"
              << "  --bytes <n>            Wire UDP packet size in [64, 1472] (default: 64)\n"
              << "  --seed <n>             Deterministic payload seed (default: 20261127)\n"
              << "  --mode <mode>          constant_rate | burst (default: constant_rate)\n"
              << "  --burst-size <n>       Packets per burst (default: 16)\n"
              << "  --burst-interval-us <n> Microseconds between bursts (default: 1000)\n"
              << "  --cpu <id>             Optional sender CPU pin id (default: -1)\n";
}

} // namespace

int main(int argc, char** argv) {
    kbnl::BenchmarkConfig cfg{};
    cfg.topology.port = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next_val = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (arg == "--target-ip") {
            cfg.topology.target_ip = next_val("--target-ip");
        } else if (arg == "--port") {
            cfg.topology.port = static_cast<uint16_t>(std::stoul(next_val("--port")));
        } else if (arg == "--pps") {
            cfg.workload.target_pps = std::stoull(next_val("--pps"));
        } else if (arg == "--warmup") {
            cfg.workload.warmup_packets = std::stoull(next_val("--warmup"));
        } else if (arg == "--count") {
            cfg.workload.measured_packets = std::stoull(next_val("--count"));
        } else if (arg == "--bytes") {
            cfg.workload.packet_bytes = std::stoull(next_val("--bytes"));
        } else if (arg == "--seed") {
            cfg.workload.seed = std::stoull(next_val("--seed"));
        } else if (arg == "--mode") {
            if (!kbnl::parse_pacing_mode(next_val("--mode"), cfg.workload.pacing_mode)) {
                std::cerr << "Invalid --mode\n";
                return 2;
            }
        } else if (arg == "--burst-size") {
            cfg.workload.burst_size = std::stoull(next_val("--burst-size"));
        } else if (arg == "--burst-interval-us") {
            cfg.workload.burst_interval_us = std::stoull(next_val("--burst-interval-us"));
        } else if (arg == "--cpu") {
            cfg.topology.sender_cpu = std::stoi(next_val("--cpu"));
            cfg.topology.pin_threads = cfg.topology.sender_cpu >= 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage();
            return 2;
        }
    }

    if (cfg.topology.port == 0) {
        std::cerr << "Error: --port is required and must be > 0\n";
        return 2;
    }

    const auto errs = kbnl::validate_config(cfg);
    if (!errs.empty()) {
        for (const auto& e : errs) {
            std::cerr << "Config error [" << e.code << "] " << e.field << ": " << e.message << "\n";
        }
        return 2;
    }

    if (cfg.topology.pin_threads) {
        const auto pin = kbnl::pin_current_thread(cfg.topology.sender_cpu);
        std::cout << "[sender] pin=" << (pin.applied ? "applied" : "not_applied")
                  << " mechanism=" << pin.mechanism << " detail=\"" << pin.detail << "\"\n";
    }

    kbnl::PosixUdpSender sender;
    const auto st = sender.open(cfg.topology, cfg.topology.port);
    if (!st.ok()) {
        std::cerr << "[sender] open failed [" << kbnl::status_code_name(st.code) << "]: " << st.message << "\n";
        return 1;
    }

    const kbnl::PacerSchedule schedule(cfg.workload);
    const uint64_t total = schedule.total_data_packets();
    std::vector<uint8_t> buf(cfg.workload.packet_bytes);

    uint64_t sent = 0;
    uint64_t errors = 0;
    const uint64_t start_ns = kbnl::now_ns();
    for (uint64_t i = 0; i < total; ++i) {
        kbnl::pace_until_ns(start_ns + schedule.scheduled_offset_ns(i));
        uint16_t flags = schedule.is_warmup(i) ? kbnl::kFlagWarmup : kbnl::kFlagNone;
        if (schedule.is_burst_packet(i)) {
            flags = static_cast<uint16_t>(flags | kbnl::kFlagBurst);
        }
        kbnl::encode_deterministic_packet(
            i + 1, kbnl::now_ns(), cfg.workload.feed_id, flags, cfg.workload.seed, cfg.workload.packet_bytes, buf);
        if (sender.send_packet(buf).ok()) {
            ++sent;
        } else {
            ++errors;
        }
    }

    for (int e = 0; e < 3; ++e) {
        kbnl::encode_deterministic_packet(
            0, kbnl::now_ns(), cfg.workload.feed_id, kbnl::kFlagEndOfStream,
            cfg.workload.seed, cfg.workload.packet_bytes, buf);
        (void)sender.send_packet(buf);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const uint64_t elapsed_ns = kbnl::now_ns() - start_ns;
    const double pps = elapsed_ns > 0 ? (static_cast<double>(sent) * 1e9 / static_cast<double>(elapsed_ns)) : 0.0;
    std::cout << "[sender] sent=" << sent << " errors=" << errors
              << " elapsed_ms=" << (static_cast<double>(elapsed_ns) / 1e6)
              << " achieved_pps=" << pps << "\n";
    return errors == 0 ? 0 : 1;
}

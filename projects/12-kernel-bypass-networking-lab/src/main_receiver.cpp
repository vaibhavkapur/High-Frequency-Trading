#include "kbnl/clock.hpp"
#include "kbnl/config.hpp"
#include "kbnl/cpu_affinity.hpp"
#include "kbnl/latency_stats.hpp"
#include "kbnl/protocol.hpp"
#include "kbnl/sequence_tracker.hpp"
#include "kbnl/transport.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void print_usage() {
    std::cerr << "Usage: kbnl_receiver [options]\n"
              << "  --backend <name>       posix_udp_blocking | posix_udp_busy_poll | posix_udp_recvmmsg | af_xdp | dpdk\n"
              << "  --bind-ip <ip>         Bind IPv4 (default: 127.0.0.1)\n"
              << "  --port <port>          UDP port (default: 9000)\n"
              << "  --interface <iface>    NIC interface name (default: lo0)\n"
              << "  --queue <id>           RX queue id (default: 0)\n"
              << "  --batch <n>            Max frames per poll (default: 32)\n"
              << "  --idle-timeout-ms <n>  Stop after idle ms once stream starts (default: 1000)\n"
              << "  --cpu <id>             Optional receiver CPU pin id (default: -1)\n";
}

} // namespace

int main(int argc, char** argv) {
    kbnl::BenchmarkConfig cfg{};
    cfg.topology.port = 9000;
    uint32_t idle_timeout_ms = 1000;

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
        } else if (arg == "--backend") {
            if (!kbnl::parse_backend_name(next_val("--backend"), cfg.workload.backend)) {
                std::cerr << "Unsupported backend name\n";
                return 2;
            }
        } else if (arg == "--bind-ip") {
            cfg.topology.bind_ip = next_val("--bind-ip");
        } else if (arg == "--port") {
            cfg.topology.port = static_cast<uint16_t>(std::stoul(next_val("--port")));
        } else if (arg == "--interface") {
            cfg.topology.interface_name = next_val("--interface");
        } else if (arg == "--queue") {
            cfg.topology.rx_queue_id = static_cast<uint32_t>(std::stoul(next_val("--queue")));
        } else if (arg == "--batch") {
            cfg.workload.batch_size = std::stoull(next_val("--batch"));
        } else if (arg == "--idle-timeout-ms") {
            idle_timeout_ms = static_cast<uint32_t>(std::stoul(next_val("--idle-timeout-ms")));
        } else if (arg == "--cpu") {
            cfg.topology.receiver_cpu = std::stoi(next_val("--cpu"));
            cfg.topology.pin_threads = cfg.topology.receiver_cpu >= 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage();
            return 2;
        }
    }

    const auto cap = kbnl::probe_backend_capability(cfg.workload.backend, cfg.topology);
    if (!cap.runtime_available) {
        std::cerr << "[receiver] backend=" << cap.backend << " status=" << cap.status
                  << " reason_code=" << kbnl::status_code_name(cap.reason_code)
                  << " reason=\"" << cap.reason << "\"\n";
        return 3;
    }

    if (cfg.topology.pin_threads) {
        const auto pin = kbnl::pin_current_thread(cfg.topology.receiver_cpu);
        std::cout << "[receiver] pin=" << (pin.applied ? "applied" : "not_applied")
                  << " mechanism=" << pin.mechanism << " detail=\"" << pin.detail << "\"\n";
    }

    kbnl::Status st = kbnl::Status::Ok();
    auto receiver = kbnl::create_receiver(cfg.workload.backend, st);
    if (!st.ok() || !receiver) {
        std::cerr << "[receiver] create failed: " << st.message << "\n";
        return 1;
    }
    st = receiver->open(cfg.topology);
    if (!st.ok()) {
        std::cerr << "[receiver] open failed [" << kbnl::status_code_name(st.code) << "]: " << st.message << "\n";
        return 1;
    }

    std::cout << "[receiver] listening backend=" << receiver->backend_label()
              << " bind_ip=" << cfg.topology.bind_ip
              << " port=" << receiver->bound_port() << "\n";

    kbnl::SequenceTracker tracker;
    kbnl::LatencyStats latency;
    std::vector<kbnl::ReceivedFrame> batch;
    batch.reserve(cfg.workload.batch_size);

    bool started = false;
    uint64_t last_rx_ns = kbnl::now_ns();
    while (true) {
        kbnl::Status poll_st = kbnl::Status::Ok();
        const size_t n = receiver->poll_batch(batch, cfg.workload.batch_size, 50, poll_st);
        if (!poll_st.ok()) {
            std::cerr << "[receiver] poll error: " << poll_st.message << "\n";
            return 1;
        }
        bool eos = false;
        if (n > 0) {
            started = true;
            last_rx_ns = kbnl::now_ns();
            for (const auto& f : batch) {
                kbnl::DecodedPacket pkt{};
                const auto derr = kbnl::decode_packet(std::span<const uint8_t>(f.data.data(), f.length), pkt);
                if (derr != kbnl::DecodeError::kOk) {
                    tracker.on_decode_error(derr);
                    continue;
                }
                const auto obs = tracker.on_packet(pkt.header);
                if (obs == kbnl::PacketObservation::kEndOfStream) {
                    eos = true;
                } else if (obs == kbnl::PacketObservation::kInOrder ||
                           obs == kbnl::PacketObservation::kGapDetected ||
                           obs == kbnl::PacketObservation::kOutOfOrder) {
                    latency.record_sample(pkt.header.send_ts_ns, f.recv_ts_ns,
                                          (pkt.header.flags & kbnl::kFlagWarmup) != 0);
                }
            }
        }
        if (eos) {
            break;
        }
        if (started && (kbnl::now_ns() - last_rx_ns) >= static_cast<uint64_t>(idle_timeout_ms) * 1000000ULL) {
            break;
        }
    }

    const auto& c = tracker.counters();
    const auto l = latency.summarize();
    std::cout << "[receiver] unique=" << c.unique_received
              << " measured=" << c.measured_received
              << " missing_within_highest=" << c.missing_within_highest
              << " ooo=" << c.out_of_order
              << " dup=" << c.duplicates
              << " decode_err=" << c.decode_errors
              << " invariant=" << (c.accounting_invariant_holds() ? "OK" : "FAIL")
              << " p50_ns=" << l.p50_ns
              << " p99_ns=" << l.p99_ns
              << " p99_9_ns=" << l.p999_ns << "\n";
    return c.accounting_invariant_holds() ? 0 : 1;
}

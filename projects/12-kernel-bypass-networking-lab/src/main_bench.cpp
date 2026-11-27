#include "kbnl/benchmark_runner.hpp"
#include "kbnl/clock.hpp"
#include "kbnl/config.hpp"
#include "kbnl/json_writer.hpp"
#include "kbnl/protocol.hpp"
#include "kbnl/transport.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

void write_u32_le(std::ofstream& ofs, uint32_t v) {
    uint8_t b[4] = {
        static_cast<uint8_t>(v & 0xFFu),
        static_cast<uint8_t>((v >> 8) & 0xFFu),
        static_cast<uint8_t>((v >> 16) & 0xFFu),
        static_cast<uint8_t>((v >> 24) & 0xFFu)
    };
    ofs.write(reinterpret_cast<const char*>(b), 4);
}

void write_u64_le(std::ofstream& ofs, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) {
        b[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFFu);
    }
    ofs.write(reinterpret_cast<const char*>(b), 8);
}

// Generates the canonical deterministic binary fixture matching fixtures/expected_reference_result.json.
bool generate_canonical_fixture(const std::string& path) {
    struct Spec {
        uint64_t seq;
        uint16_t flags;
        uint64_t send_ts_ns;
        uint64_t recv_ts_ns;
        bool corrupt_payload;
    };

    // Expected total sent = 12 (seq 1..12).
    // seq 4 dropped; seq 6 delayed after seq 8; seq 7 duplicated; seq 9 checksum corrupted.
    const std::vector<Spec> specs = {
        {1,  kbnl::kFlagWarmup,       1000,  1500, false},
        {2,  kbnl::kFlagWarmup,       2000,  2500, false},
        {3,  kbnl::kFlagNone,         3000,  4000, false}, // 1000 ns
        {5,  kbnl::kFlagNone,         5000,  6200, false}, // 1200 ns (gap: missing 4)
        {7,  kbnl::kFlagNone,         7000,  8400, false}, // 1400 ns (gap: missing 6)
        {8,  kbnl::kFlagNone,         8000,  9600, false}, // 1600 ns
        {6,  kbnl::kFlagNone,         6000,  9000, false}, // 3000 ns (out-of-order fill)
        {7,  kbnl::kFlagNone,         7000,  9000, false}, // duplicate (excluded from latency)
        {9,  kbnl::kFlagNone,         9000, 10500, true},  // corrupted checksum
        {10, kbnl::kFlagNone,        10000, 11800, false}, // 1800 ns (gap: missing 9)
        {11, kbnl::kFlagNone,        11000, 13200, false}, // 2200 ns
        {12, kbnl::kFlagNone,        12000, 14500, false}, // 2500 ns
        {0,  kbnl::kFlagEndOfStream, 13000, 13500, false}  // EOS
    };

    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return false;
    }

    const char magic[8] = {'K', 'B', 'N', 'L', 'F', 'I', 'X', '1'};
    ofs.write(magic, 8);
    write_u32_le(ofs, static_cast<uint32_t>(specs.size()));
    write_u32_le(ofs, 12u); // expected_total_sent

    constexpr uint64_t kSeed = 20261127ULL;
    constexpr size_t kBytes = 64;
    std::vector<uint8_t> pkt(kBytes);
    for (const auto& s : specs) {
        kbnl::encode_deterministic_packet(s.seq, s.send_ts_ns, 1, s.flags, kSeed, kBytes, pkt);
        if (s.corrupt_payload) {
            pkt[kBytes - 1] ^= 0xFFu;
        }
        write_u64_le(ofs, s.recv_ts_ns);
        write_u32_le(ofs, static_cast<uint32_t>(pkt.size()));
        ofs.write(reinterpret_cast<const char*>(pkt.data()), static_cast<std::streamsize>(pkt.size()));
    }
    return ofs.good();
}

void print_usage() {
    std::cerr << "Usage: kbnl_bench [options]\n"
              << "  --config <file.json>        Run benchmark from JSON config\n"
              << "  --backend <name>            Override backend in config\n"
              << "  --output <file.json>        Write raw JSON report to file\n"
              << "  --probe <backend>           Probe backend capability and emit JSON report\n"
              << "  --interface <iface>         Interface name for --probe (default: lo0)\n"
              << "  --replay-fixture <file.bin> Replay deterministic binary fixture\n"
              << "  --generate-fixture <file.bin> Generate canonical deterministic fixture\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path;
    std::string backend_override;
    std::string output_path;
    std::string probe_backend;
    std::string probe_iface = "lo0";
    std::string replay_fixture;
    std::string gen_fixture;

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
        } else if (arg == "--config") {
            config_path = next_val("--config");
        } else if (arg == "--backend") {
            backend_override = next_val("--backend");
        } else if (arg == "--output") {
            output_path = next_val("--output");
        } else if (arg == "--probe") {
            probe_backend = next_val("--probe");
        } else if (arg == "--interface") {
            probe_iface = next_val("--interface");
        } else if (arg == "--replay-fixture") {
            replay_fixture = next_val("--replay-fixture");
        } else if (arg == "--generate-fixture") {
            gen_fixture = next_val("--generate-fixture");
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage();
            return 2;
        }
    }

    if (!gen_fixture.empty()) {
        if (!generate_canonical_fixture(gen_fixture)) {
            std::cerr << "Failed to write fixture: " << gen_fixture << "\n";
            return 1;
        }
        std::cout << "Generated canonical fixture at " << gen_fixture << "\n";
        return 0;
    }

    if (!probe_backend.empty()) {
        kbnl::TransportBackend backend{};
        if (!kbnl::parse_backend_name(probe_backend, backend)) {
            std::cerr << "Unsupported backend: " << probe_backend << "\n";
            return 2;
        }
        kbnl::TopologyConfig topo{};
        topo.interface_name = probe_iface;
        const auto rep = kbnl::probe_backend_capability(backend, topo);
        const auto json = kbnl::serialize_capability_report_json(rep, kbnl::get_clock_metadata());
        if (!output_path.empty()) {
            std::string err;
            if (!kbnl::write_text_file(output_path, json, err)) {
                std::cerr << err << "\n";
                return 1;
            }
        } else {
            std::cout << json;
        }
        return 0;
    }

    if (!replay_fixture.empty()) {
        const auto res = kbnl::run_fixture_replay(replay_fixture);
        const auto json = kbnl::serialize_benchmark_result_json(res);
        if (!output_path.empty()) {
            std::string err;
            if (!kbnl::write_text_file(output_path, json, err)) {
                std::cerr << err << "\n";
                return 1;
            }
        } else {
            std::cout << json;
        }
        return (res.status.ok() && res.sequence_accounting_verified) ? 0 : 1;
    }

    kbnl::BenchmarkConfig cfg{};
    if (!config_path.empty()) {
        std::vector<kbnl::ValidationError> errs;
        if (!kbnl::load_config_from_json_file(config_path, cfg, errs)) {
            for (const auto& e : errs) {
                std::cerr << "Config validation error [" << e.code << "] "
                          << e.field << ": " << e.message << "\n";
            }
            return 2;
        }
    }

    if (!backend_override.empty()) {
        if (!kbnl::parse_backend_name(backend_override, cfg.workload.backend)) {
            std::cerr << "Unsupported backend override: " << backend_override << "\n";
            return 2;
        }
    }

    const auto res = kbnl::run_benchmark(cfg);
    const auto json = kbnl::serialize_benchmark_result_json(res);
    const std::string final_out = !output_path.empty() ? output_path : cfg.output_path;
    if (!final_out.empty()) {
        std::string err;
        if (!kbnl::write_text_file(final_out, json, err)) {
            std::cerr << err << "\n";
            return 1;
        }
    } else {
        std::cout << json;
    }

    if (res.execution_status == "FAILED") {
        std::cerr << "Benchmark failed [" << kbnl::status_code_name(res.status.code) << "]: "
                  << res.status.message << "\n";
        return 1;
    }
    return 0;
}

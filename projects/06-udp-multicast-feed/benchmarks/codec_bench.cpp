#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "mdf/exchange_simulator.hpp"
#include "mdf/protocol.hpp"

using namespace mdf;

namespace {

uint64_t percentile(std::vector<uint64_t> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const std::size_t idx =
      std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size())));
  return v[idx];
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t iterations = 200000;
  std::size_t warmup = 20000;
  for (int i = 1; i + 1 < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--iterations") iterations = std::stoull(argv[++i]);
    else if (arg == "--warmup") warmup = std::stoull(argv[++i]);
  }

  std::cout << "=== MDF1 Wire Codec + CRC32 Microbenchmark ===\n"
            << "Warmup: " << warmup << " | Measured iterations: " << iterations << "\n\n"
            << std::left << std::setw(14) << "events/pkt" << std::setw(10) << "wire_B"
            << std::setw(14) << "enc_Mpps" << std::setw(14) << "dec_Mpps" << std::setw(14)
            << "dec_Mev/s" << std::setw(12) << "enc_p50_ns" << std::setw(12) << "dec_p50_ns"
            << std::setw(12) << "dec_p99_ns" << "\n";

  for (std::size_t batch : {std::size_t{1}, std::size_t{4}, std::size_t{16}, std::size_t{32}}) {
    ExchangeSimulator sim({42, {1001, 1002, 1003, 1004}, 10000, 64});
    const auto events = sim.next_batch(batch);
    PacketHeader hdr{};
    hdr.channel_id = 1;
    hdr.seq_num = 1;
    hdr.send_ts_ns = 123456;

    std::vector<uint8_t> wire;
    DecodedPacket decoded{};
    decoded.events.reserve(batch);

    for (std::size_t i = 0; i < warmup; ++i) {
      hdr.seq_num = i + 1;
      (void)encode_packet(hdr, events, wire);
      (void)decode_packet(wire, decoded);
    }

    std::vector<uint64_t> enc_ns;
    std::vector<uint64_t> dec_ns;
    enc_ns.reserve(iterations);
    dec_ns.reserve(iterations);

    uint64_t sink = 0;
    const uint64_t enc_start = steady_now_ns();
    for (std::size_t i = 0; i < iterations; ++i) {
      hdr.seq_num = i + 1;
      const uint64_t t0 = steady_now_ns();
      (void)encode_packet(hdr, events, wire);
      const uint64_t t1 = steady_now_ns();
      enc_ns.push_back(t1 - t0);
      sink += wire.size();
    }
    const uint64_t enc_elapsed_ns = steady_now_ns() - enc_start;

    const uint64_t dec_start = steady_now_ns();
    for (std::size_t i = 0; i < iterations; ++i) {
      const uint64_t t0 = steady_now_ns();
      (void)decode_packet(wire, decoded);
      const uint64_t t1 = steady_now_ns();
      dec_ns.push_back(t1 - t0);
      sink += decoded.events.size();
    }
    const uint64_t dec_elapsed_ns = steady_now_ns() - dec_start;

    if (sink == 0) std::cerr << "unreachable\n";

    const double enc_mpps =
        (static_cast<double>(iterations) / (static_cast<double>(enc_elapsed_ns) / 1e9)) / 1e6;
    const double dec_mpps =
        (static_cast<double>(iterations) / (static_cast<double>(dec_elapsed_ns) / 1e9)) / 1e6;
    const double dec_mevs = dec_mpps * static_cast<double>(batch);

    std::cout << std::left << std::setw(14) << batch << std::setw(10) << wire.size() << std::fixed
              << std::setprecision(2) << std::setw(14) << enc_mpps << std::setw(14) << dec_mpps
              << std::setw(14) << dec_mevs << std::setw(12) << percentile(enc_ns, 0.50)
              << std::setw(12) << percentile(dec_ns, 0.50) << std::setw(12)
              << percentile(dec_ns, 0.99) << "\n";
  }

  return 0;
}

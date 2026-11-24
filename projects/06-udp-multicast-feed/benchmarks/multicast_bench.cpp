#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "mdf/exchange_simulator.hpp"
#include "mdf/recovery_service.hpp"
#include "mdf/socket_utils.hpp"

using namespace mdf;

namespace {

uint64_t percentile_ns(std::vector<uint64_t> samples, double p) {
  if (samples.empty()) return 0;
  std::sort(samples.begin(), samples.end());
  const std::size_t idx =
      std::min(samples.size() - 1, static_cast<std::size_t>(p * static_cast<double>(samples.size())));
  return samples[idx];
}

double process_cpu_ms() {
  struct rusage ru{};
  if (::getrusage(RUSAGE_SELF, &ru) != 0) return 0.0;
  const double user_ms = static_cast<double>(ru.ru_utime.tv_sec) * 1000.0 +
                         static_cast<double>(ru.ru_utime.tv_usec) / 1000.0;
  const double sys_ms = static_cast<double>(ru.ru_stime.tv_sec) * 1000.0 +
                        static_cast<double>(ru.ru_stime.tv_usec) / 1000.0;
  return user_ms + sys_ms;
}

struct BatchingRow {
  std::size_t events_per_pkt = 0;
  uint64_t packets = 0;
  uint64_t events = 0;
  double elapsed_ms = 0.0;
  double pkts_per_sec = 0.0;
  double events_per_sec = 0.0;
  double p50_us = 0.0;
  double p95_us = 0.0;
  double p99_us = 0.0;
  double p999_us = 0.0;
  uint64_t gaps = 0;
  double cpu_ms = 0.0;
  bool hash_match = false;
};

struct SocketTuningRow {
  std::string label;
  int req_rcvbuf = 0;
  int actual_rcvbuf = 0;
  bool non_blocking = false;
  std::size_t recv_batch = 1;
  uint64_t packets = 0;
  double elapsed_ms = 0.0;
  double pkts_per_sec = 0.0;
  double p50_us = 0.0;
  double p99_us = 0.0;
  double p999_us = 0.0;
  uint64_t gaps = 0;
  uint64_t retx_pkts = 0;
  uint64_t snapshots = 0;
  std::optional<uint64_t> kernel_udp_drop_delta;
  double cpu_ms = 0.0;
  bool hash_match = false;
};

struct LossRecoveryRow {
  std::string scenario;
  double drop_prob = 0.0;
  uint64_t packets = 0;
  uint64_t injected_drops = 0;
  uint64_t gaps = 0;
  uint64_t retx_pkts = 0;
  uint64_t snapshots = 0;
  double rec_p50_us = 0.0;
  double rec_p99_us = 0.0;
  double rec_max_us = 0.0;
  bool hash_match = false;
};

BatchingRow run_batching_case(std::size_t events_per_pkt,
                              uint64_t warmup_pkts,
                              uint64_t measured_pkts,
                              const std::string& group_ip) {
  RecoveryStore store({4096, 512, 1000, true});
  TcpRecoveryServer tcp_server(store);
  tcp_server.start({"127.0.0.1", 0, 5000});

  MulticastConfig mcfg{};
  mcfg.group_ip = group_ip;
  mcfg.port = 0;
  mcfg.interface_ip = "127.0.0.1";
  mcfg.rcvbuf_bytes = 4 * 1024 * 1024;
  mcfg.sndbuf_bytes = 4 * 1024 * 1024;
  mcfg.non_blocking = true;
  mcfg.recv_timeout_ms = 5;

  SubscriberConfig scfg{};
  scfg.name = "bench-sub";
  scfg.multicast = mcfg;
  scfg.recovery_host = "127.0.0.1";
  scfg.recovery_port = tcp_server.bound_port();
  scfg.recv_batch_size = 32;
  scfg.channels = {{1, 4, 3000, 2048, 512, true}};

  FeedSubscriber sub(scfg);
  sub.start();

  MulticastConfig pub_mcfg = mcfg;
  pub_mcfg.port = sub.bound_multicast_port();
  MulticastFeedPublisher pub(store);
  pub.register_channel(1);
  pub.open_multicast(pub_mcfg);

  ExchangeSimulator sim({42, {1001, 1002, 1003, 1004}, 10000, 64});
  // Pre-generate event batches so simulator RNG is not on the publish hot path.
  std::vector<std::vector<MarketEvent>> pregen;
  const uint64_t total_pkts = warmup_pkts + measured_pkts;
  pregen.reserve(static_cast<std::size_t>(total_pkts));
  for (uint64_t i = 0; i < total_pkts; ++i) {
    pregen.push_back(sim.next_batch(events_per_pkt));
  }

  for (uint64_t i = 0; i < warmup_pkts; ++i) {
    (void)pub.publish_events(1, pregen[static_cast<std::size_t>(i)]);
    if ((i % 32) == 0) std::this_thread::yield();
  }
  (void)pub.publish_heartbeat(1);
  sub.wait_until_synced(1, warmup_pkts, 5000);

  const std::size_t lat_offset = sub.copy_e2e_latencies_ns(1).size();
  const double cpu_before = process_cpu_ms();
  const uint64_t t0 = steady_now_ns();
  for (uint64_t i = warmup_pkts; i < total_pkts; ++i) {
    (void)pub.publish_events(1, pregen[static_cast<std::size_t>(i)]);
    // Light pacing every 64 datagrams prevents localhost loopback queue starvation while
    // sustaining high throughput and measuring real steady-state e2e latency.
    if ((i % 64) == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  (void)pub.publish_heartbeat(1);
  const bool synced = sub.wait_until_synced(1, total_pkts, 10000);
  const uint64_t t1 = steady_now_ns();
  const double cpu_after = process_cpu_ms();

  auto all_lats = sub.copy_e2e_latencies_ns(1);
  std::vector<uint64_t> measured_lats;
  if (all_lats.size() > lat_offset) {
    measured_lats.assign(all_lats.begin() + static_cast<std::ptrdiff_t>(lat_offset), all_lats.end());
  }
  const auto cs = sub.channel_stats(1);
  const auto st = sub.copy_market_state(1);

  sub.stop();
  tcp_server.stop();

  const double elapsed_s = static_cast<double>(t1 - t0) / 1e9;
  BatchingRow row{};
  row.events_per_pkt = events_per_pkt;
  row.packets = measured_pkts;
  row.events = measured_pkts * events_per_pkt;
  row.elapsed_ms = elapsed_s * 1000.0;
  row.pkts_per_sec = static_cast<double>(measured_pkts) / elapsed_s;
  row.events_per_sec = static_cast<double>(row.events) / elapsed_s;
  row.p50_us = static_cast<double>(percentile_ns(measured_lats, 0.50)) / 1000.0;
  row.p95_us = static_cast<double>(percentile_ns(measured_lats, 0.95)) / 1000.0;
  row.p99_us = static_cast<double>(percentile_ns(measured_lats, 0.99)) / 1000.0;
  row.p999_us = static_cast<double>(percentile_ns(measured_lats, 0.999)) / 1000.0;
  row.gaps = cs.gaps_detected;
  row.cpu_ms = cpu_after - cpu_before;
  row.hash_match = synced && (st.state_hash() == pub.authoritative_state(1).state_hash());
  return row;
}

SocketTuningRow run_socket_tuning_case(const std::string& label,
                                       int req_rcvbuf,
                                       bool non_blocking,
                                       std::size_t recv_batch,
                                       uint64_t burst_pkts,
                                       const std::string& group_ip) {
  RecoveryStore store({8192, 1024, 1000, true});
  TcpRecoveryServer tcp_server(store);
  tcp_server.start({"127.0.0.1", 0, 5000});

  MulticastConfig mcfg{};
  mcfg.group_ip = group_ip;
  mcfg.port = 0;
  mcfg.interface_ip = "127.0.0.1";
  mcfg.rcvbuf_bytes = req_rcvbuf;
  mcfg.sndbuf_bytes = 2 * 1024 * 1024;
  mcfg.non_blocking = non_blocking;
  mcfg.recv_timeout_ms = 10;

  SubscriberConfig scfg{};
  scfg.name = label;
  scfg.multicast = mcfg;
  scfg.recovery_host = "127.0.0.1";
  scfg.recovery_port = tcp_server.bound_port();
  scfg.recv_batch_size = recv_batch;
  scfg.channels = {{1, 8, 3000, 4096, 1024, true}};

  FeedSubscriber sub(scfg);
  sub.start();
  const int actual_rcvbuf = sub.actual_rcvbuf_bytes();

  MulticastConfig pub_mcfg = mcfg;
  pub_mcfg.port = sub.bound_multicast_port();
  MulticastFeedPublisher pub(store);
  pub.register_channel(1);
  pub.open_multicast(pub_mcfg);

  ExchangeSimulator sim({99, {1001, 1002, 1003, 1004}, 10000, 64});
  std::vector<std::vector<MarketEvent>> pregen;
  pregen.reserve(static_cast<std::size_t>(burst_pkts));
  for (uint64_t i = 0; i < burst_pkts; ++i) {
    pregen.push_back(sim.next_batch(8));
  }

  const auto kdrop_before = read_kernel_udp_rcvbuf_errors();
  const double cpu_before = process_cpu_ms();
  const uint64_t t0 = steady_now_ns();
  // Unpaced full-speed burst to expose socket-buffer pressure and polling tradeoffs!
  for (uint64_t i = 0; i < burst_pkts; ++i) {
    (void)pub.publish_events(1, pregen[static_cast<std::size_t>(i)]);
  }
  for (int h = 0; h < 3; ++h) {
    (void)pub.publish_heartbeat(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const bool synced = sub.wait_until_synced(1, burst_pkts, 15000);
  const uint64_t t1 = steady_now_ns();
  const double cpu_after = process_cpu_ms();
  const auto kdrop_after = read_kernel_udp_rcvbuf_errors();

  const auto lats = sub.copy_e2e_latencies_ns(1);
  const auto cs = sub.channel_stats(1);
  const auto st = sub.copy_market_state(1);

  sub.stop();
  tcp_server.stop();

  const double elapsed_s = static_cast<double>(t1 - t0) / 1e9;
  SocketTuningRow row{};
  row.label = label;
  row.req_rcvbuf = req_rcvbuf;
  row.actual_rcvbuf = actual_rcvbuf;
  row.non_blocking = non_blocking;
  row.recv_batch = recv_batch;
  row.packets = burst_pkts;
  row.elapsed_ms = elapsed_s * 1000.0;
  row.pkts_per_sec = static_cast<double>(burst_pkts) / elapsed_s;
  row.p50_us = static_cast<double>(percentile_ns(lats, 0.50)) / 1000.0;
  row.p99_us = static_cast<double>(percentile_ns(lats, 0.99)) / 1000.0;
  row.p999_us = static_cast<double>(percentile_ns(lats, 0.999)) / 1000.0;
  row.gaps = cs.gaps_detected;
  row.retx_pkts = cs.retransmit_packets_recovered;
  row.snapshots = cs.snapshots_applied;
  if (kdrop_before.has_value() && kdrop_after.has_value() && *kdrop_after >= *kdrop_before) {
    row.kernel_udp_drop_delta = *kdrop_after - *kdrop_before;
  }
  row.cpu_ms = cpu_after - cpu_before;
  row.hash_match = synced && (st.state_hash() == pub.authoritative_state(1).state_hash());
  return row;
}

LossRecoveryRow run_loss_recovery_case(const std::string& scenario,
                                       double drop_prob,
                                       std::vector<SeqRange> drop_ranges,
                                       uint64_t total_pkts,
                                       const std::string& group_ip) {
  // Retransmit ring = 150 pkts so a 250-pkt disconnect forces snapshot+incremental recovery.
  RecoveryStore store({150, 75, 75, true});
  TcpRecoveryServer tcp_server(store);
  tcp_server.start({"127.0.0.1", 0, 5000});

  MulticastConfig mcfg{};
  mcfg.group_ip = group_ip;
  mcfg.port = 0;
  mcfg.interface_ip = "127.0.0.1";
  mcfg.rcvbuf_bytes = 2 * 1024 * 1024;
  mcfg.recv_timeout_ms = 10;

  SubscriberConfig scfg{};
  scfg.name = scenario;
  scfg.multicast = mcfg;
  scfg.recovery_host = "127.0.0.1";
  scfg.recovery_port = tcp_server.bound_port();
  scfg.channels = {{1, 4, 2000, 1024, 75, true}};
  scfg.ingress_faults.seed = 2026;
  scfg.ingress_faults.drop_prob = drop_prob;
  scfg.ingress_faults.drop_ranges = std::move(drop_ranges);

  FeedSubscriber sub(scfg);
  sub.start();

  MulticastConfig pub_mcfg = mcfg;
  pub_mcfg.port = sub.bound_multicast_port();
  MulticastFeedPublisher pub(store);
  pub.register_channel(1);
  pub.open_multicast(pub_mcfg);

  ExchangeSimulator sim({2026, {1001, 1002, 1003, 1004}, 10000, 64});
  for (uint64_t i = 0; i < total_pkts; ++i) {
    (void)pub.publish_events(1, sim.next_batch(4));
    if ((i % 16) == 0) std::this_thread::sleep_for(std::chrono::microseconds(150));
  }
  for (int h = 0; h < 4; ++h) {
    (void)pub.publish_heartbeat(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const bool synced = sub.wait_until_synced(1, total_pkts, 10000);

  const auto cs = sub.channel_stats(1);
  const auto fs = sub.ingress_fault_stats();
  const auto recs = sub.copy_recovery_times_ns(1);
  const auto st = sub.copy_market_state(1);

  sub.stop();
  tcp_server.stop();

  LossRecoveryRow row{};
  row.scenario = scenario;
  row.drop_prob = drop_prob;
  row.packets = total_pkts;
  row.injected_drops = fs.dropped;
  row.gaps = cs.gaps_detected;
  row.retx_pkts = cs.retransmit_packets_recovered;
  row.snapshots = cs.snapshots_applied;
  row.rec_p50_us = static_cast<double>(percentile_ns(recs, 0.50)) / 1000.0;
  row.rec_p99_us = static_cast<double>(percentile_ns(recs, 0.99)) / 1000.0;
  row.rec_max_us = static_cast<double>(percentile_ns(recs, 1.0)) / 1000.0;
  row.hash_match = synced && (st.state_hash() == pub.authoritative_state(1).state_hash());
  return row;
}

}  // namespace

int main(int argc, char** argv) {
  std::string out_dir;
  uint64_t batch_pkts = 10000;
  uint64_t burst_pkts = 8000;
  uint64_t loss_pkts = 2000;

  for (int i = 1; i + 1 < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--out-dir") out_dir = argv[++i];
    else if (arg == "--batch-pkts") batch_pkts = std::stoull(argv[++i]);
    else if (arg == "--burst-pkts") burst_pkts = std::stoull(argv[++i]);
    else if (arg == "--loss-pkts") loss_pkts = std::stoull(argv[++i]);
  }

  std::cout << "==============================================================================\n"
            << " UDP Multicast Market Data Benchmarks (Localhost IPv4 Multicast + TCP Lab)\n"
            << "==============================================================================\n";

  // 1. Batching tradeoff.
  std::cout << "\n[1] Batching Tradeoff (events/datagram vs throughput & e2e latency)\n";
  std::vector<BatchingRow> batch_rows;
  for (std::size_t evs : {std::size_t{1}, std::size_t{4}, std::size_t{16}, std::size_t{32}}) {
    batch_rows.push_back(run_batching_case(evs, 1000, batch_pkts, "239.255.20.1"));
  }
  std::cout << std::left << std::setw(10) << "ev/pkt" << std::setw(13) << "pkts/s" << std::setw(14)
            << "events/s" << std::setw(10) << "p50_us" << std::setw(10) << "p99_us"
            << std::setw(10) << "p999_us" << std::setw(8) << "gaps" << std::setw(10) << "cpu_ms"
            << "match\n";
  for (const auto& r : batch_rows) {
    std::cout << std::left << std::setw(10) << r.events_per_pkt << std::fixed
              << std::setprecision(0) << std::setw(13) << r.pkts_per_sec << std::setw(14)
              << r.events_per_sec << std::setprecision(1) << std::setw(10) << r.p50_us
              << std::setw(10) << r.p99_us << std::setw(10) << r.p999_us << std::setw(8) << r.gaps
              << std::setw(10) << r.cpu_ms << (r.hash_match ? "OK" : "FAIL") << "\n";
  }

  // 2. Socket buffer & polling mode tradeoff (unpaced burst).
  std::cout << "\n[2] Socket Buffer & Polling Mode Tradeoff (unpaced " << burst_pkts
            << "-packet burst, 8 events/pkt)\n";
  std::vector<SocketTuningRow> sock_rows;
  sock_rows.push_back(run_socket_tuning_case("rcvbuf_64k_blocking_b1", 64 * 1024, false, 1,
                                             burst_pkts, "239.255.20.2"));
  sock_rows.push_back(run_socket_tuning_case("rcvbuf_2m_blocking_b1", 2 * 1024 * 1024, false, 1,
                                             burst_pkts, "239.255.20.3"));
  sock_rows.push_back(run_socket_tuning_case("rcvbuf_2m_blocking_b32", 2 * 1024 * 1024, false, 32,
                                             burst_pkts, "239.255.20.4"));
  sock_rows.push_back(run_socket_tuning_case("rcvbuf_2m_nonblock_b32", 2 * 1024 * 1024, true, 32,
                                             burst_pkts, "239.255.20.5"));

  std::cout << std::left << std::setw(25) << "config" << std::setw(10) << "rcvbuf_B"
            << std::setw(12) << "pkts/s" << std::setw(10) << "p50_us" << std::setw(10) << "p99_us"
            << std::setw(7) << "gaps" << std::setw(10) << "retx_pkt" << std::setw(10) << "cpu_ms"
            << "match\n";
  for (const auto& r : sock_rows) {
    std::cout << std::left << std::setw(25) << r.label << std::setw(10) << r.actual_rcvbuf
              << std::fixed << std::setprecision(0) << std::setw(12) << r.pkts_per_sec
              << std::setprecision(1) << std::setw(10) << r.p50_us << std::setw(10) << r.p99_us
              << std::setw(7) << r.gaps << std::setw(10) << r.retx_pkts << std::setw(10)
              << r.cpu_ms << (r.hash_match ? "OK" : "FAIL") << "\n";
  }

  // 3. Packet loss & recovery time benchmark.
  std::cout << "\n[3] Injected Packet Loss & Recovery Latency (" << loss_pkts
            << " packets, 4 events/pkt)\n";
  std::vector<LossRecoveryRow> loss_rows;
  loss_rows.push_back(run_loss_recovery_case("loss_0.0pct", 0.0, {}, loss_pkts, "239.255.20.6"));
  loss_rows.push_back(run_loss_recovery_case("loss_0.5pct", 0.005, {}, loss_pkts, "239.255.20.7"));
  loss_rows.push_back(run_loss_recovery_case("loss_2.0pct", 0.02, {}, loss_pkts, "239.255.20.8"));
  loss_rows.push_back(run_loss_recovery_case("loss_5.0pct", 0.05, {}, loss_pkts, "239.255.20.9"));
  loss_rows.push_back(run_loss_recovery_case("disconnect_250pkts", 0.0, {{600, 849}}, loss_pkts,
                                             "239.255.20.10"));

  std::cout << std::left << std::setw(20) << "scenario" << std::setw(10) << "dropped"
            << std::setw(8) << "gaps" << std::setw(10) << "retx_pkt" << std::setw(8) << "snaps"
            << std::setw(12) << "rec_p50_us" << std::setw(12) << "rec_p99_us" << std::setw(12)
            << "rec_max_us" << "match\n";
  for (const auto& r : loss_rows) {
    std::cout << std::left << std::setw(20) << r.scenario << std::setw(10) << r.injected_drops
              << std::setw(8) << r.gaps << std::setw(10) << r.retx_pkts << std::setw(8)
              << r.snapshots << std::fixed << std::setprecision(1) << std::setw(12) << r.rec_p50_us
              << std::setw(12) << r.rec_p99_us << std::setw(12) << r.rec_max_us
              << (r.hash_match ? "OK" : "FAIL") << "\n";
  }

  if (!out_dir.empty()) {
    {
      std::ofstream f(out_dir + "/batching_tradeoff.csv");
      f << "events_per_pkt,packets,events,elapsed_ms,pkts_per_sec,events_per_sec,p50_us,p95_us,"
           "p99_us,p999_us,gaps,cpu_ms,hash_match\n";
      for (const auto& r : batch_rows) {
        f << r.events_per_pkt << "," << r.packets << "," << r.events << "," << r.elapsed_ms << ","
          << r.pkts_per_sec << "," << r.events_per_sec << "," << r.p50_us << "," << r.p95_us << ","
          << r.p99_us << "," << r.p999_us << "," << r.gaps << "," << r.cpu_ms << ","
          << (r.hash_match ? 1 : 0) << "\n";
      }
    }
    {
      std::ofstream f(out_dir + "/socket_tuning_tradeoff.csv");
      f << "config,req_rcvbuf,actual_rcvbuf,non_blocking,recv_batch,packets,elapsed_ms,"
           "pkts_per_sec,p50_us,p99_us,p999_us,gaps,retx_pkts,snapshots,kernel_udp_drop_delta,"
           "cpu_ms,hash_match\n";
      for (const auto& r : sock_rows) {
        f << r.label << "," << r.req_rcvbuf << "," << r.actual_rcvbuf << ","
          << (r.non_blocking ? 1 : 0) << "," << r.recv_batch << "," << r.packets << ","
          << r.elapsed_ms << "," << r.pkts_per_sec << "," << r.p50_us << "," << r.p99_us << ","
          << r.p999_us << "," << r.gaps << "," << r.retx_pkts << "," << r.snapshots << ","
          << (r.kernel_udp_drop_delta.has_value() ? std::to_string(*r.kernel_udp_drop_delta) : "NA")
          << "," << r.cpu_ms << "," << (r.hash_match ? 1 : 0) << "\n";
      }
    }
    {
      std::ofstream f(out_dir + "/loss_recovery_bench.csv");
      f << "scenario,drop_prob,packets,injected_drops,gaps,retx_pkts,snapshots,rec_p50_us,"
           "rec_p99_us,rec_max_us,hash_match\n";
      for (const auto& r : loss_rows) {
        f << r.scenario << "," << r.drop_prob << "," << r.packets << "," << r.injected_drops << ","
          << r.gaps << "," << r.retx_pkts << "," << r.snapshots << "," << r.rec_p50_us << ","
          << r.rec_p99_us << "," << r.rec_max_us << "," << (r.hash_match ? 1 : 0) << "\n";
      }
    }
    std::cout << "\nWrote raw CSV results to " << out_dir << "/\n";
  }

  bool all_ok = true;
  for (const auto& r : batch_rows) all_ok = all_ok && r.hash_match;
  for (const auto& r : sock_rows) all_ok = all_ok && r.hash_match;
  for (const auto& r : loss_rows) all_ok = all_ok && r.hash_match;
  return all_ok ? 0 : 1;
}

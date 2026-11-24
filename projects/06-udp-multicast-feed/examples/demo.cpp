#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "mdf/exchange_simulator.hpp"
#include "mdf/recovery_service.hpp"

using namespace mdf;

namespace {

uint64_t percentile_ns(std::vector<uint64_t> samples, double p) {
  if (samples.empty()) return 0;
  std::sort(samples.begin(), samples.end());
  const std::size_t idx =
      std::min(samples.size() - 1, static_cast<std::size_t>(p * static_cast<double>(samples.size())));
  return samples[idx];
}

void print_subscriber_report(const std::string& label,
                             const FeedSubscriber& sub,
                             uint16_t channel_id,
                             const ChannelMarketState& ref) {
  const auto st = sub.copy_market_state(channel_id);
  const auto cs = sub.channel_stats(channel_id);
  const auto fs = sub.ingress_fault_stats();
  const auto lats = sub.copy_e2e_latencies_ns(channel_id);
  const auto recs = sub.copy_recovery_times_ns(channel_id);
  const bool match = (st == ref) && (st.state_hash() == ref.state_hash());

  std::cout << "\n--- " << label << " ---\n"
            << "  State:             " << receiver_state_name(sub.receiver_state(channel_id))
            << " | last_seq=" << st.last_applied_seq()
            << " | active_orders=" << st.total_active_orders() << "\n"
            << "  Book Hash:         0x" << std::hex << std::setw(16) << std::setfill('0')
            << st.state_hash() << std::dec << std::setfill(' ')
            << (match ? "  [MATCHES EXCHANGE]" : "  [MISMATCH!]") << "\n"
            << "  Datagrams/Applied: rx=" << cs.datagrams_processed
            << " applied_pkts=" << cs.packets_applied << " events=" << cs.events_applied
            << " heartbeats=" << cs.heartbeats_received << "\n"
            << "  Anomalies:         gaps=" << cs.gaps_detected
            << " ooo=" << cs.out_of_order_packets << " reordered_ok=" << cs.reordered_recovered_packets
            << " dups=" << cs.duplicate_packets << " crc_err=" << cs.checksum_errors << "\n"
            << "  Faults Injected:   dropped=" << fs.dropped << " reordered=" << fs.reordered
            << " dup=" << fs.duplicated << " corrupt=" << fs.corrupted << "\n"
            << "  TCP Recovery:      retx_req=" << cs.retransmit_requests
            << " retx_pkts=" << cs.retransmit_packets_recovered
            << " retx_unavail=" << cs.retransmit_unavailable_responses
            << " snapshots=" << cs.snapshots_applied
            << " stale_discarded=" << cs.stale_incrementals_discarded << "\n"
            << "  E2E Latency (us):  p50=" << std::fixed << std::setprecision(1)
            << (static_cast<double>(percentile_ns(lats, 0.50)) / 1000.0)
            << " p99=" << (static_cast<double>(percentile_ns(lats, 0.99)) / 1000.0);
  if (!recs.empty()) {
    std::cout << " | Recovery (us): p50="
              << (static_cast<double>(percentile_ns(recs, 0.50)) / 1000.0)
              << " max=" << (static_cast<double>(percentile_ns(recs, 1.0)) / 1000.0);
  }
  std::cout << "\n";
}

}  // namespace

int main() {
  std::cout << "====================================================================\n"
            << " UDP Multicast Market Data Distribution & Recovery — Live Lab Demo\n"
            << "====================================================================\n";

  // Retransmit ring holds 100 packets; checkpoint every 50 packets.
  RecoveryStore store({100, 50, 50, true});
  TcpRecoveryServer tcp_server(store);
  std::string err;
  if (!tcp_server.start({"127.0.0.1", 0, 5000}, &err)) {
    std::cerr << "Failed to start TCP recovery server: " << err << "\n";
    return 1;
  }

  MulticastConfig mcfg{};
  mcfg.group_ip = "239.255.1.10";
  mcfg.port = 0;  // Ephemeral port
  mcfg.interface_ip = "127.0.0.1";
  mcfg.recv_timeout_ms = 15;
  mcfg.rcvbuf_bytes = 2 * 1024 * 1024;

  // Subscriber 1: Uninterrupted reference feed handler.
  SubscriberConfig cfg_ref{};
  cfg_ref.name = "sub-reference";
  cfg_ref.multicast = mcfg;
  cfg_ref.recovery_host = "127.0.0.1";
  cfg_ref.recovery_port = tcp_server.bound_port();
  cfg_ref.channels = {{1, 4, 3000, 512, 50, true}};

  FeedSubscriber sub_ref(cfg_ref);
  if (!sub_ref.start(&err)) {
    std::cerr << "Failed to start reference subscriber: " << err << "\n";
    return 1;
  }
  const uint16_t mcast_port = sub_ref.bound_multicast_port();

  // Subscriber 2: Experiences 2% packet drops, 8% reordering, 5% duplicates, 1% CRC corruption.
  SubscriberConfig cfg_lossy = cfg_ref;
  cfg_lossy.name = "sub-lossy-retransmit";
  cfg_lossy.multicast.port = mcast_port;
  cfg_lossy.ingress_faults.seed = 2026;
  cfg_lossy.ingress_faults.drop_prob = 0.02;
  cfg_lossy.ingress_faults.reorder_prob = 0.08;
  cfg_lossy.ingress_faults.max_reorder_delay = 3;
  cfg_lossy.ingress_faults.duplicate_prob = 0.05;
  cfg_lossy.ingress_faults.corrupt_prob = 0.01;
  FeedSubscriber sub_lossy(cfg_lossy);
  if (!sub_lossy.start(&err)) {
    std::cerr << "Failed to start lossy subscriber: " << err << "\n";
    return 1;
  }

  // Subscriber 3: Suffers a 220-packet disconnect [200..420] exceeding the 100-packet
  // retransmit ring -> forces TCP Snapshot + Incremental Synchronization!
  SubscriberConfig cfg_disc = cfg_ref;
  cfg_disc.name = "sub-disconnect-snapshot";
  cfg_disc.multicast.port = mcast_port;
  cfg_disc.ingress_faults.seed = 9999;
  cfg_disc.ingress_faults.drop_ranges = {{200, 420}};
  FeedSubscriber sub_disc(cfg_disc);
  if (!sub_disc.start(&err)) {
    std::cerr << "Failed to start disconnect subscriber: " << err << "\n";
    return 1;
  }

  MulticastConfig pub_mcfg = mcfg;
  pub_mcfg.port = mcast_port;
  MulticastFeedPublisher pub(store);
  pub.register_channel(1);
  if (!pub.open_multicast(pub_mcfg, &err)) {
    std::cerr << "Failed to open multicast publisher: " << err << "\n";
    return 1;
  }

  std::cout << "Multicast group:     " << pub_mcfg.group_ip << ":" << pub_mcfg.port << " (iface "
            << pub_mcfg.interface_ip << ")\n"
            << "TCP Recovery Server: 127.0.0.1:" << tcp_server.bound_port()
            << " (retx_ring=100 pkts, checkpoint_interval=50 pkts)\n"
            << "Socket buffers:      SNDBUF=" << pub.actual_sndbuf_bytes()
            << " B | RCVBUF=" << sub_ref.actual_rcvbuf_bytes() << " B\n"
            << "Publishing 600 sequenced packets (4 events/packet = 2,400 market events)...\n";

  ExchangeSimulator sim({1337, {1001, 1002, 1003, 1004}, 10000, 64});
  constexpr uint64_t kTotalPackets = 600;
  for (uint64_t i = 0; i < kTotalPackets; ++i) {
    (void)pub.publish_events(1, sim.next_batch(4));
    if ((i % 10) == 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(250));
    }
  }
  for (int h = 0; h < 5; ++h) {
    (void)pub.publish_heartbeat(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  const bool ok_ref = sub_ref.wait_until_synced(1, kTotalPackets, 5000);
  const bool ok_lossy = sub_lossy.wait_until_synced(1, kTotalPackets, 5000);
  const bool ok_disc = sub_disc.wait_until_synced(1, kTotalPackets, 5000);

  const auto& ref = pub.authoritative_state(1);
  std::cout << "\nExchange Authoritative State: seq=" << ref.last_applied_seq()
            << " | active_orders=" << ref.total_active_orders() << " | hash=0x" << std::hex
            << std::setw(16) << std::setfill('0') << ref.state_hash() << std::dec
            << std::setfill(' ') << "\n";

  print_subscriber_report("Subscriber 1: Uninterrupted Reference", sub_ref, 1, ref);
  print_subscriber_report("Subscriber 2: Loss/Reorder/Dup/Corrupt (TCP Retransmit)", sub_lossy, 1,
                          ref);
  print_subscriber_report("Subscriber 3: Induced 221-Packet Disconnect (TCP Snapshot+Incremental)",
                          sub_disc, 1, ref);

  const auto rstats = store.stats();
  std::cout << "\n--- Recovery Server Summary ---\n"
            << "  Packets recorded: " << rstats.packets_recorded
            << " | Evicted: " << rstats.packets_evicted
            << " | Replay checkpoints: " << rstats.checkpoints_created << "\n"
            << "  Retransmit reqs:  " << rstats.retransmit_requests
            << " (ok=" << rstats.retransmit_ok << ", unavail=" << rstats.retransmit_unavailable
            << ", pkts_sent=" << rstats.retransmit_packets_sent << ")\n"
            << "  Snapshot reqs:    " << rstats.snapshot_requests
            << " (sent=" << rstats.snapshots_sent << ")\n";

  const bool all_equal = ok_ref && ok_lossy && ok_disc &&
                         (sub_ref.copy_market_state(1) == ref) &&
                         (sub_lossy.copy_market_state(1) == ref) &&
                         (sub_disc.copy_market_state(1) == ref);

  sub_ref.stop();
  sub_lossy.stop();
  sub_disc.stop();
  tcp_server.stop();

  std::cout << "\nConvergence Verification: "
            << (all_equal ? "ALL 3 SUBSCRIBERS MATCH EXCHANGE BOOK EXACTLY (PASS)" : "FAILED")
            << "\n";
  return all_equal ? 0 : 1;
}

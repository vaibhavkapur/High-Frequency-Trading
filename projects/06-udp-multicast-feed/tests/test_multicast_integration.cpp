#include "mdf/exchange_simulator.hpp"
#include "mdf/multicast_transport.hpp"
#include "mdf/recovery_service.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace mdf;

MDF_TEST(M1_SingleMulticastPublisherAndReceiver) {
  RecoveryStore store({512, 128, 100, true});
  TcpRecoveryServer tcp_server(store);
  std::string err;
  EXPECT_TRUE(tcp_server.start({"127.0.0.1", 0, 3000}, &err));
  if (!err.empty()) std::cerr << "TCP server error: " << err << "\n";

  MulticastConfig mcfg{};
  mcfg.group_ip = "239.255.10.1";
  mcfg.port = 0;  // Ephemeral port chosen by first receiver
  mcfg.interface_ip = "127.0.0.1";
  mcfg.recv_timeout_ms = 20;

  SubscriberConfig scfg{};
  scfg.name = "m1-sub";
  scfg.multicast = mcfg;
  scfg.recovery_host = "127.0.0.1";
  scfg.recovery_port = tcp_server.bound_port();
  scfg.channels = {{1, 4, 5000, 512, 128, true}};

  FeedSubscriber sub(scfg);
  EXPECT_TRUE(sub.start(&err));
  if (!err.empty()) std::cerr << "Subscriber start error: " << err << "\n";

  MulticastConfig pub_mcfg = mcfg;
  pub_mcfg.port = sub.bound_multicast_port();

  MulticastFeedPublisher pub(store);
  pub.register_channel(1);
  EXPECT_TRUE(pub.open_multicast(pub_mcfg, &err));
  if (!err.empty()) std::cerr << "Publisher open error: " << err << "\n";

  ExchangeSimulator sim({111, {1001, 1002}, 10000, 32});
  constexpr uint64_t kPackets = 200;
  for (uint64_t i = 0; i < kPackets; ++i) {
    EXPECT_GT(pub.publish_events(1, sim.next_batch(4)), 0ULL);
    if ((i % 25) == 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }
  EXPECT_TRUE(pub.publish_heartbeat(1));

  EXPECT_TRUE(sub.wait_until_synced(1, kPackets, 5000));
  sub.stop();
  tcp_server.stop();

  const auto sub_state = sub.copy_market_state(1);
  EXPECT_TRUE(sub_state == pub.authoritative_state(1));
  EXPECT_EQ(sub_state.state_hash(), pub.authoritative_state(1).state_hash());
}

MDF_TEST(M2_M3_M4_MultiSubscriberConvergenceUnderFaultsAndDisconnect) {
  // Small retransmit ring (80 packets) so a 200-packet disconnect forces real TCP snapshot sync!
  RecoveryStore store({80, 40, 40, true});
  TcpRecoveryServer tcp_server(store);
  std::string err;
  EXPECT_TRUE(tcp_server.start({"127.0.0.1", 0, 5000}, &err));
  EXPECT_GT(tcp_server.bound_port(), 0U);

  MulticastConfig mcfg{};
  mcfg.group_ip = "239.255.10.2";
  mcfg.port = 0;
  mcfg.interface_ip = "127.0.0.1";
  mcfg.recv_timeout_ms = 15;
  mcfg.rcvbuf_bytes = 4 * 1024 * 1024;

  // Subscriber A: Uninterrupted reference subscriber.
  SubscriberConfig cfg_a{};
  cfg_a.name = "sub-clean";
  cfg_a.multicast = mcfg;
  cfg_a.recovery_host = "127.0.0.1";
  cfg_a.recovery_port = tcp_server.bound_port();
  cfg_a.channels = {{1, 4, 5000, 512, 40, true}};

  FeedSubscriber sub_a(cfg_a);
  EXPECT_TRUE(sub_a.start(&err));
  const uint16_t mcast_port = sub_a.bound_multicast_port();
  EXPECT_GT(mcast_port, 0U);

  // Subscriber B: Joins the exact same multicast group + port (SO_REUSEPORT), experiences
  // random packet drops, reordering, duplicates, bit corruption AND a 200-packet disconnect!
  SubscriberConfig cfg_b = cfg_a;
  cfg_b.name = "sub-faulted";
  cfg_b.multicast.port = mcast_port;
  cfg_b.ingress_faults.seed = 4242;
  cfg_b.ingress_faults.drop_prob = 0.03;
  cfg_b.ingress_faults.reorder_prob = 0.08;
  cfg_b.ingress_faults.max_reorder_delay = 3;
  cfg_b.ingress_faults.duplicate_prob = 0.05;
  cfg_b.ingress_faults.corrupt_prob = 0.02;
  cfg_b.ingress_faults.drop_ranges = {{150, 350}};  // Exceeds 80-pkt retransmit ring -> snapshot!

  FeedSubscriber sub_b(cfg_b);
  EXPECT_TRUE(sub_b.start(&err));

  // Subscriber C: Third subscriber sharing the same group/port with only light drops (retransmit).
  SubscriberConfig cfg_c = cfg_a;
  cfg_c.name = "sub-retransmit";
  cfg_c.multicast.port = mcast_port;
  cfg_c.ingress_faults.seed = 9001;
  cfg_c.ingress_faults.drop_ranges = {{30, 35}, {250, 255}, {440, 442}};

  FeedSubscriber sub_c(cfg_c);
  EXPECT_TRUE(sub_c.start(&err));

  MulticastConfig pub_mcfg = mcfg;
  pub_mcfg.port = mcast_port;
  MulticastFeedPublisher pub(store);
  pub.register_channel(1);
  EXPECT_TRUE(pub.open_multicast(pub_mcfg, &err));

  ExchangeSimulator sim({2026, {1001, 1002, 1003, 1004}, 10000, 64});
  constexpr uint64_t kPackets = 500;
  for (uint64_t i = 0; i < kPackets; ++i) {
    EXPECT_GT(pub.publish_events(1, sim.next_batch(4)), 0ULL);
    if ((i % 10) == 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
  }
  // Emit a few heartbeats so any delayed tail packets or tail drops are flushed & detected.
  for (int h = 0; h < 5; ++h) {
    EXPECT_TRUE(pub.publish_heartbeat(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_TRUE(sub_a.wait_until_synced(1, kPackets, 5000));
  EXPECT_TRUE(sub_b.wait_until_synced(1, kPackets, 5000));
  EXPECT_TRUE(sub_c.wait_until_synced(1, kPackets, 5000));

  const auto state_a = sub_a.copy_market_state(1);
  const auto state_b = sub_b.copy_market_state(1);
  const auto state_c = sub_c.copy_market_state(1);
  const auto stats_b = sub_b.channel_stats(1);
  const auto stats_c = sub_c.channel_stats(1);

  sub_a.stop();
  sub_b.stop();
  sub_c.stop();
  tcp_server.stop();

  const auto& ref = pub.authoritative_state(1);
  EXPECT_TRUE(state_a == ref);
  EXPECT_TRUE(state_b == ref);
  EXPECT_TRUE(state_c == ref);
  EXPECT_EQ(state_a.state_hash(), ref.state_hash());
  EXPECT_EQ(state_b.state_hash(), ref.state_hash());
  EXPECT_EQ(state_c.state_hash(), ref.state_hash());

  // Verify Subscriber B genuinely suffered gaps, checksum errors, and recovered via snapshot+TCP.
  EXPECT_GT(stats_b.gaps_detected, 0ULL);
  EXPECT_GT(stats_b.checksum_errors, 0ULL);
  EXPECT_GT(stats_b.retransmit_unavailable_responses, 0ULL);
  EXPECT_GT(stats_b.snapshots_applied, 0ULL);

  // Verify Subscriber C recovered purely via retransmission (no snapshot needed).
  EXPECT_GT(stats_c.gaps_detected, 0ULL);
  EXPECT_GT(stats_c.retransmit_packets_recovered, 0ULL);
  EXPECT_EQ(stats_c.snapshots_applied, 0ULL);
}

MDF_TEST_MAIN()

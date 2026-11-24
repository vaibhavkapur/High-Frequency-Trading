#include "mdf/channel_receiver.hpp"
#include "mdf/exchange_simulator.hpp"
#include "mdf/fault_injector.hpp"
#include "mdf/recovery_service.hpp"
#include "test_framework.hpp"

#include <vector>

using namespace mdf;

MDF_TEST(RetransmitRecoveryMatchesUninterruptedReceiver) {
  RecoveryStore store({256, 64, 100, true});
  InMemoryRecoveryClient recovery(store);
  MulticastFeedPublisher pub(store);
  pub.register_channel(1);

  ChannelReceiver clean_rx({1, 4, 2000, 256, 64, false}, &recovery);
  ChannelReceiver fault_rx({1, 4, 2000, 256, 64, false}, &recovery);

  FaultConfig fcfg{};
  fcfg.seed = 101;
  fcfg.drop_ranges = {{10, 12}, {55, 65}, {199, 200}};
  FaultInjector fault(fcfg);

  uint64_t sim_clock_ns = 1'000'000;
  pub.set_custom_sink([&](std::span<const uint8_t> dgram) {
    sim_clock_ns += 10'000;
    (void)clean_rx.on_datagram(dgram, sim_clock_ns);
    for (const auto& item : fault.process(dgram)) {
      (void)fault_rx.on_datagram(item.bytes, sim_clock_ns);
    }
    return true;
  });

  ExchangeSimulator sim({42, {1001, 1002, 1003}, 10000, 40});
  for (int i = 0; i < 200; ++i) {
    EXPECT_GT(pub.publish_events(1, sim.next_batch(4)), 0ULL);
  }
  EXPECT_TRUE(pub.publish_heartbeat(1));
  EXPECT_TRUE(fault_rx.flush_recovery(sim_clock_ns + 5'000'000));

  EXPECT_EQ(clean_rx.state(), ReceiverState::kLive);
  EXPECT_EQ(fault_rx.state(), ReceiverState::kLive);
  EXPECT_GT(fault_rx.stats().retransmit_requests, 0ULL);
  EXPECT_EQ(fault_rx.stats().retransmit_packets_recovered, 16ULL);  // 3 + 11 + 2
  EXPECT_EQ(fault_rx.stats().snapshots_applied, 0ULL);

  EXPECT_TRUE(clean_rx.market_state() == pub.authoritative_state(1));
  EXPECT_TRUE(fault_rx.market_state() == clean_rx.market_state());
  EXPECT_EQ(fault_rx.market_state().state_hash(), clean_rx.market_state().state_hash());
}

MDF_TEST(DeepDisconnectEscalatesToSnapshotAndIncrementalSync) {
  // Small retransmit buffer (64 packets) so a 200-packet disconnect is NOT recoverable via
  // retransmit and MUST escalate to snapshot + incremental replay!
  RecoveryStore store({64, 32, 50, true});
  InMemoryRecoveryClient recovery(store);
  MulticastFeedPublisher pub(store);
  pub.register_channel(1);

  ChannelReceiver clean_rx({1, 4, 2000, 256, 32, false}, &recovery);
  ChannelReceiver disconnected_rx({1, 4, 2000, 256, 32, false}, &recovery);

  FaultConfig fcfg{};
  fcfg.seed = 202;
  fcfg.drop_ranges = {{80, 280}};
  FaultInjector fault(fcfg);

  uint64_t sim_clock_ns = 1'000'000;
  pub.set_custom_sink([&](std::span<const uint8_t> dgram) {
    sim_clock_ns += 10'000;
    (void)clean_rx.on_datagram(dgram, sim_clock_ns);
    for (const auto& item : fault.process(dgram)) {
      (void)disconnected_rx.on_datagram(item.bytes, sim_clock_ns);
    }
    return true;
  });

  ExchangeSimulator sim({777, {1001, 1002, 1003, 1004}, 25000, 50});
  for (int i = 0; i < 350; ++i) {
    EXPECT_GT(pub.publish_events(1, sim.next_batch(5)), 0ULL);
  }
  EXPECT_TRUE(pub.publish_heartbeat(1));
  EXPECT_TRUE(disconnected_rx.flush_recovery(sim_clock_ns + 5'000'000));

  EXPECT_EQ(disconnected_rx.state(), ReceiverState::kLive);
  EXPECT_GE(disconnected_rx.stats().retransmit_unavailable_responses, 1ULL);
  EXPECT_GE(disconnected_rx.stats().snapshots_applied, 1ULL);
  EXPECT_TRUE(disconnected_rx.market_state() == clean_rx.market_state());
  EXPECT_EQ(disconnected_rx.market_state().state_hash(), clean_rx.market_state().state_hash());
  EXPECT_EQ(disconnected_rx.market_state().state_hash(), pub.authoritative_state(1).state_hash());
}

MDF_TEST(CombinedChaosFaultsConvergeDeterministically) {
  auto run_once = [](uint64_t seed) {
    RecoveryStore store({128, 64, 60, true});
    InMemoryRecoveryClient recovery(store);
    MulticastFeedPublisher pub(store);
    pub.register_channel(5);

    ChannelReceiver clean_rx({5, 4, 2000, 512, 64, false}, &recovery);
    ChannelReceiver chaos_rx({5, 4, 2000, 512, 64, false}, &recovery);

    FaultConfig fcfg{};
    fcfg.seed = seed;
    fcfg.drop_prob = 0.05;
    fcfg.reorder_prob = 0.15;
    fcfg.max_reorder_delay = 5;
    fcfg.duplicate_prob = 0.10;
    fcfg.corrupt_prob = 0.03;
    fcfg.drop_ranges = {{400, 580}};  // Force a mid-run snapshot sync too!
    FaultInjector fault(fcfg);

    uint64_t sim_clock_ns = 1'000'000;
    pub.set_custom_sink([&](std::span<const uint8_t> dgram) {
      sim_clock_ns += 5'000;
      (void)clean_rx.on_datagram(dgram, sim_clock_ns);
      for (const auto& item : fault.process(dgram)) {
        (void)chaos_rx.on_datagram(item.bytes, sim_clock_ns);
      }
      return true;
    });

    ExchangeSimulator sim({seed, {1001, 1002, 1003, 1004}, 10000, 64});
    for (int i = 0; i < 1000; ++i) {
      EXPECT_GT(pub.publish_events(5, sim.next_batch(4)), 0ULL);
    }
    for (const auto& item : fault.flush()) {
      sim_clock_ns += 5'000;
      (void)chaos_rx.on_datagram(item.bytes, sim_clock_ns);
    }
    EXPECT_TRUE(pub.publish_heartbeat(5));
    EXPECT_TRUE(chaos_rx.flush_recovery(sim_clock_ns + 5'000'000));

    EXPECT_EQ(chaos_rx.state(), ReceiverState::kLive);
    EXPECT_GT(chaos_rx.stats().checksum_errors, 0ULL);
    EXPECT_GT(chaos_rx.stats().duplicate_packets, 0ULL);
    EXPECT_GT(chaos_rx.stats().out_of_order_packets, 0ULL);
    EXPECT_GT(chaos_rx.stats().snapshots_applied, 0ULL);
    EXPECT_TRUE(chaos_rx.market_state() == clean_rx.market_state());
    EXPECT_EQ(chaos_rx.market_state().state_hash(), pub.authoritative_state(5).state_hash());
    return chaos_rx.market_state().state_hash();
  };

  // Repeatability: same seed produces identical final state hash.
  const uint64_t h1 = run_once(2026);
  const uint64_t h2 = run_once(2026);
  EXPECT_EQ(h1, h2);
}

MDF_TEST(MultiChannelIsolation) {
  RecoveryStore store({128, 64, 50, true});
  InMemoryRecoveryClient recovery(store);
  MulticastFeedPublisher pub(store);
  pub.register_channel(10);
  pub.register_channel(20);

  ChannelReceiver rx10({10, 3, 2000, 128, 64, false}, &recovery);
  ChannelReceiver rx20({20, 3, 2000, 128, 64, false}, &recovery);

  FaultConfig f10{};
  f10.seed = 9;
  f10.drop_prob = 0.15;
  FaultInjector fault10(f10);

  uint64_t ts = 1'000'000;
  pub.set_custom_sink([&](std::span<const uint8_t> dgram) {
    ts += 1000;
    PacketHeader hdr{};
    if (decode_packet_header(dgram, hdr) == DecodeStatus::kOk) {
      if (hdr.channel_id == 10) {
        for (const auto& item : fault10.process(dgram)) {
          (void)rx10.on_datagram(item.bytes, ts);
        }
      } else if (hdr.channel_id == 20) {
        (void)rx20.on_datagram(dgram, ts);
      }
    }
    return true;
  });

  ExchangeSimulator sim10({10, {1001, 1002}, 10000, 20});
  ExchangeSimulator sim20({20, {2001, 2002}, 50000, 20});
  for (int i = 0; i < 150; ++i) {
    EXPECT_GT(pub.publish_events(10, sim10.next_batch(3)), 0ULL);
    EXPECT_GT(pub.publish_events(20, sim20.next_batch(3)), 0ULL);
  }
  EXPECT_TRUE(pub.publish_heartbeat(10));
  EXPECT_TRUE(rx10.flush_recovery(ts + 5'000'000));

  EXPECT_EQ(rx20.stats().gaps_detected, 0ULL);
  EXPECT_GT(rx10.stats().gaps_detected, 0ULL);
  EXPECT_TRUE(rx10.market_state() == pub.authoritative_state(10));
  EXPECT_TRUE(rx20.market_state() == pub.authoritative_state(20));
}

MDF_TEST_MAIN()

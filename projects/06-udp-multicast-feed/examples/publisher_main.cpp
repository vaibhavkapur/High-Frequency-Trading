#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

#include "mdf/exchange_simulator.hpp"
#include "mdf/recovery_service.hpp"

using namespace mdf;

namespace {

void print_usage(const char* prog) {
  std::cout << "Usage: " << prog << " [options]\n"
            << "  --group <ip>         Multicast group IP (default 239.255.0.1)\n"
            << "  --port <port>        Multicast UDP port (default 30001)\n"
            << "  --iface <ip>         Interface IP (default 127.0.0.1)\n"
            << "  --recovery-port <p>  TCP recovery server port (default 30002)\n"
            << "  --channel <id>       Channel ID (default 1)\n"
            << "  --packets <n>        Number of packets to publish (default 1000)\n"
            << "  --batch <n>          Events per packet (default 4)\n"
            << "  --interval-us <us>   Microsecond pacing between packets (default 100)\n"
            << "  --drop-prob <p>      Publisher fault-injector drop probability (default 0.0)\n"
            << "  --reorder-prob <p>   Publisher fault-injector reorder probability (default 0.0)\n"
            << "  --hold-ms <ms>       Keep TCP recovery server alive after burst (default 2000)\n";
}

}  // namespace

int main(int argc, char** argv) {
  MulticastConfig mcfg{};
  uint16_t recovery_port = 30002;
  uint16_t channel_id = 1;
  uint64_t total_packets = 1000;
  std::size_t events_per_packet = 4;
  uint64_t interval_us = 100;
  int hold_ms = 2000;
  FaultConfig fault{};

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        std::exit(1);
      }
      return argv[++i];
    };
    if (arg == "--group") mcfg.group_ip = next();
    else if (arg == "--port") mcfg.port = static_cast<uint16_t>(std::stoul(next()));
    else if (arg == "--iface") mcfg.interface_ip = next();
    else if (arg == "--recovery-port") recovery_port = static_cast<uint16_t>(std::stoul(next()));
    else if (arg == "--channel") channel_id = static_cast<uint16_t>(std::stoul(next()));
    else if (arg == "--packets") total_packets = std::stoull(next());
    else if (arg == "--batch") events_per_packet = std::stoull(next());
    else if (arg == "--interval-us") interval_us = std::stoull(next());
    else if (arg == "--drop-prob") fault.drop_prob = std::stod(next());
    else if (arg == "--reorder-prob") fault.reorder_prob = std::stod(next());
    else if (arg == "--hold-ms") hold_ms = std::stoi(next());
    else {
      print_usage(argv[0]);
      return 0;
    }
  }

  RecoveryStore store({1024, 256, 200, true});
  TcpRecoveryServer tcp_server(store);
  std::string err;
  if (!tcp_server.start({"127.0.0.1", recovery_port, 5000}, &err)) {
    std::cerr << "TCP recovery server failed: " << err << "\n";
    return 1;
  }

  MulticastFeedPublisher pub(store, fault);
  pub.register_channel(channel_id);
  if (!pub.open_multicast(mcfg, &err)) {
    std::cerr << "Multicast sender open failed: " << err << "\n";
    return 1;
  }

  std::cout << "Publishing " << total_packets << " packets (" << events_per_packet
            << " events/pkt) to " << mcfg.group_ip << ":" << mcfg.port
            << " | TCP recovery on 127.0.0.1:" << tcp_server.bound_port() << "\n";

  ExchangeSimulator sim({1337, {1001, 1002, 1003, 1004}, 10000, 64});
  for (uint64_t i = 0; i < total_packets; ++i) {
    (void)pub.publish_events(channel_id, sim.next_batch(events_per_packet));
    if (interval_us > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(interval_us));
    }
  }
  pub.flush_fault_injector();
  for (int h = 0; h < 3; ++h) {
    (void)pub.publish_heartbeat(channel_id);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  const auto& ref = pub.authoritative_state(channel_id);
  std::cout << "Published seq=1.." << ref.last_applied_seq()
            << " | events=" << pub.events_published()
            << " | hash=0x" << std::hex << std::setw(16) << std::setfill('0') << ref.state_hash()
            << std::dec << std::setfill(' ') << "\n";

  if (hold_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
  }
  tcp_server.stop();
  return 0;
}

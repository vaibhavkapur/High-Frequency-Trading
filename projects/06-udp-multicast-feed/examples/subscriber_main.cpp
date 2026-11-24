#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

#include "mdf/exchange_simulator.hpp"

using namespace mdf;

namespace {

void print_usage(const char* prog) {
  std::cout << "Usage: " << prog << " [options]\n"
            << "  --group <ip>         Multicast group IP (default 239.255.0.1)\n"
            << "  --port <port>        Multicast UDP port (default 30001)\n"
            << "  --iface <ip>         Interface IP (default 127.0.0.1)\n"
            << "  --recovery-host <h>  TCP recovery host (default 127.0.0.1)\n"
            << "  --recovery-port <p>  TCP recovery port (default 30002)\n"
            << "  --channel <id>       Channel ID (default 1)\n"
            << "  --target-seq <n>     Stop once synced to seq >= target-seq (default 1000)\n"
            << "  --timeout-ms <ms>    Max wait time in ms (default 15000)\n"
            << "  --drop-prob <p>      Subscriber ingress drop probability (default 0.0)\n";
}

}  // namespace

int main(int argc, char** argv) {
  SubscriberConfig scfg{};
  scfg.name = "cli-sub";
  scfg.multicast.group_ip = "239.255.0.1";
  scfg.multicast.port = 30001;
  scfg.multicast.interface_ip = "127.0.0.1";
  scfg.recovery_host = "127.0.0.1";
  scfg.recovery_port = 30002;
  uint16_t channel_id = 1;
  uint64_t target_seq = 1000;
  int timeout_ms = 15000;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        std::exit(1);
      }
      return argv[++i];
    };
    if (arg == "--group") scfg.multicast.group_ip = next();
    else if (arg == "--port") scfg.multicast.port = static_cast<uint16_t>(std::stoul(next()));
    else if (arg == "--iface") scfg.multicast.interface_ip = next();
    else if (arg == "--recovery-host") scfg.recovery_host = next();
    else if (arg == "--recovery-port")
      scfg.recovery_port = static_cast<uint16_t>(std::stoul(next()));
    else if (arg == "--channel") channel_id = static_cast<uint16_t>(std::stoul(next()));
    else if (arg == "--target-seq") target_seq = std::stoull(next());
    else if (arg == "--timeout-ms") timeout_ms = std::stoi(next());
    else if (arg == "--drop-prob") scfg.ingress_faults.drop_prob = std::stod(next());
    else {
      print_usage(argv[0]);
      return 0;
    }
  }

  scfg.channels = {{channel_id, 4, 3000, 1024, 256, true}};
  FeedSubscriber sub(scfg);
  std::string err;
  if (!sub.start(&err)) {
    std::cerr << "Failed to start subscriber: " << err << "\n";
    return 1;
  }

  std::cout << "Subscribed to " << scfg.multicast.group_ip << ":" << scfg.multicast.port
            << " (channel=" << channel_id << ", waiting for seq=" << target_seq << ")...\n";

  const bool synced = sub.wait_until_synced(channel_id, target_seq, timeout_ms);
  const auto st = sub.copy_market_state(channel_id);
  const auto cs = sub.channel_stats(channel_id);
  sub.stop();

  std::cout << "Result: " << (synced ? "SYNCED" : "TIMEOUT")
            << " | last_seq=" << st.last_applied_seq()
            << " | events=" << cs.events_applied
            << " | gaps=" << cs.gaps_detected
            << " | retx_pkts=" << cs.retransmit_packets_recovered
            << " | snapshots=" << cs.snapshots_applied
            << " | hash=0x" << std::hex << std::setw(16) << std::setfill('0') << st.state_hash()
            << std::dec << std::setfill(' ') << "\n";
  return synced ? 0 : 1;
}

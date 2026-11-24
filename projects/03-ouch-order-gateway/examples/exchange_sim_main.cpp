// Stand-alone exchange simulator, for pointing a gateway (or tcpdump/Wireshark)
// at a separate process. Listens on the loopback interface only.
//
// Usage: ouch_exchange_sim [--port N] [--user U --pass P] [--symbols A,B,...]
//                          [--fault-rate R] [--seed S]
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "ouch/sim/exchange.hpp"

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }
}  // namespace

int main(int argc, char** argv) {
  ouch::sim::SimConfig cfg;
  cfg.port = 9100;
  std::string user = "user";
  std::string pass = "pass";
  double fault_rate = 0.0;
  std::uint64_t seed = 1;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto value = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--port") {
      cfg.port = static_cast<std::uint16_t>(std::atoi(value()));
    } else if (a == "--user") {
      user = value();
    } else if (a == "--pass") {
      pass = value();
    } else if (a == "--fault-rate") {
      fault_rate = std::atof(value());
    } else if (a == "--seed") {
      seed = std::strtoull(value(), nullptr, 10);
    } else if (a == "--symbols") {
      std::string list = value();
      std::size_t at = 0;
      while (at <= list.size()) {
        const std::size_t comma = list.find(',', at);
        const std::string sym = list.substr(at, comma == std::string::npos ? comma : comma - at);
        if (!sym.empty()) cfg.symbols.push_back(sym);
        if (comma == std::string::npos) break;
        at = comma + 1;
      }
    } else {
      std::fprintf(stderr,
                   "usage: %s [--port N] [--user U --pass P] [--symbols A,B] "
                   "[--fault-rate R] [--seed S]\n",
                   argv[0]);
      return 2;
    }
  }
  cfg.accounts = {{user, pass}};
  cfg.faults = ouch::sim::FaultConfig::uniform(fault_rate, seed);

  ouch::sim::ExchangeSim sim(cfg);
  if (!sim.start()) {
    std::fprintf(stderr, "cannot listen on 127.0.0.1:%u\n", cfg.port);
    return 1;
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::printf("OUCH-style exchange simulator on 127.0.0.1:%u (account %s, fault rate %.3f)\n",
              sim.port(), user.c_str(), fault_rate);
  std::printf("Ctrl-C to stop.\n");
  std::fflush(stdout);
  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));

  sim.stop();
  const auto s = sim.stats();
  std::printf(
      "\ninbound=%llu accepted=%llu rejected=%llu executions=%llu cancels=%llu "
      "replaces=%llu duplicate_tokens=%llu ignored=%llu malformed=%llu logins=%llu\n",
      static_cast<unsigned long long>(s.inbound_messages),
      static_cast<unsigned long long>(s.accepted), static_cast<unsigned long long>(s.rejected),
      static_cast<unsigned long long>(s.executions), static_cast<unsigned long long>(s.cancels),
      static_cast<unsigned long long>(s.replaces),
      static_cast<unsigned long long>(s.duplicate_tokens),
      static_cast<unsigned long long>(s.ignored), static_cast<unsigned long long>(s.malformed),
      static_cast<unsigned long long>(s.logins));
  return 0;
}

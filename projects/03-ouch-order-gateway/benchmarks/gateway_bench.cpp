// Gateway load and fault benchmark over loopback TCP.
//
// The exchange simulator runs on its own thread in this process; the gateway
// runs on the main thread. Everything measured is localhost-only and says
// nothing about a real venue or a real network.
//
// What is measured
//   * submit-to-ack RTT: from just before the request is journaled and written
//     to the socket until the Accepted message has been read, decoded and
//     applied to the order manager (Order::acked_ns - Order::sent_ns). The
//     figure therefore includes risk-free gateway overhead, two loopback hops,
//     simulator matching and the scheduler wake-ups on both threads.
//   * throughput: requests written per second of wall time over the timed
//     section, and acknowledged orders per second.
//   * gateway thread CPU time / wall time.
//   * queue depth: requests awaiting a response, sampled at each submit.
//
// Workload ("event mix"), by order index i:
//   i % 50 == 25  oversized order, refused by pre-trade risk (never sent)
//   i % 10 == 9   passive buy $1 below the market, canceled once acknowledged
//   otherwise     alternating buy/sell at one price, so each pair trades
//
// Usage: gateway_bench [--orders N] [--warmup N] [--window W] [--rate R]
//                      [--spin 0|1] [--nodelay 0|1] [--journal none|buffered|flush]
//                      [--fault-rate P] [--seed S] [--label NAME]
//                      [--json path] [--samples path]
#include <pthread.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "bench_util.hpp"
#include "ouch/gateway.hpp"
#include "ouch/sim/exchange.hpp"

using namespace ouch;

namespace {

struct Options {
  std::size_t orders = 200'000;
  std::size_t warmup = 20'000;
  std::size_t window = 1;  // max requests awaiting a response
  double rate = 0;         // orders per second; 0 = as fast as the window allows
  bool spin = false;       // busy-poll instead of blocking in poll()
  bool nodelay = true;
  std::string journal = "none";
  double fault_rate = 0;
  std::uint64_t seed = 1;
  std::string label = "run";
  std::string json;
  std::string samples;
};

Options parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (i + 1 >= argc) {
      std::fprintf(stderr, "missing value for %s\n", a.c_str());
      std::exit(2);
    }
    const char* v = argv[++i];
    if (a == "--orders") {
      o.orders = std::strtoull(v, nullptr, 10);
    } else if (a == "--warmup") {
      o.warmup = std::strtoull(v, nullptr, 10);
    } else if (a == "--window") {
      o.window = std::strtoull(v, nullptr, 10);
    } else if (a == "--rate") {
      o.rate = std::atof(v);
    } else if (a == "--spin") {
      o.spin = std::atoi(v) != 0;
    } else if (a == "--nodelay") {
      o.nodelay = std::atoi(v) != 0;
    } else if (a == "--journal") {
      o.journal = v;
    } else if (a == "--fault-rate") {
      o.fault_rate = std::atof(v);
    } else if (a == "--seed") {
      o.seed = std::strtoull(v, nullptr, 10);
    } else if (a == "--label") {
      o.label = v;
    } else if (a == "--json") {
      o.json = v;
    } else if (a == "--samples") {
      o.samples = v;
    } else {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (o.window == 0) o.window = 1;
  if (o.warmup >= o.orders) o.warmup = o.orders / 10;
  return o;
}

std::uint64_t thread_cpu_ns() {
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

std::uint64_t process_cpu_ns() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  const auto ns = [](const timeval& t) {
    return static_cast<std::uint64_t>(t.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(t.tv_usec) * 1'000ULL;
  };
  return ns(ru.ru_utime) + ns(ru.ru_stime);
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse(argc, argv);
  constexpr std::uint32_t kPx = 1'500'000;
  const Symbol sym("AAPL");

  sim::SimConfig sc;
  sc.tcp_nodelay = opt.nodelay;
  sc.reorder_flush_ms = 2;
  // Faults are switched on only after login so every run starts the same way.
  sim::ExchangeSim exchange(sc);
  if (!exchange.start()) {
    std::fprintf(stderr, "cannot start exchange simulator\n");
    return 1;
  }

  GatewayConfig gc;
  gc.port = exchange.port();
  gc.tcp_nodelay = opt.nodelay;
  gc.reconnect_backoff_ms = 1;
  gc.reconnect_backoff_max_ms = 8;
  std::string journal_path;
  if (opt.journal != "none") {
    const char* tmp = std::getenv("TMPDIR");
    journal_path = std::string(tmp != nullptr ? tmp : "/tmp") + "/ouch_bench_" +
                   std::to_string(::getpid()) + ".journal";
    std::remove(journal_path.c_str());
    gc.journal_path = journal_path;
    gc.journal_flush_each = opt.journal == "flush";
  }
  RiskLimits limits;
  limits.max_order_qty = 1'000;
  limits.max_notional = UINT64_MAX;
  limits.require_reference_price = false;
  limits.max_long = INT64_MAX / 4;
  limits.max_short = INT64_MAX / 4;
  Gateway gw(gc, limits);

  // RTT samples for orders past the warm-up, and cancels to send from the main
  // loop (keeping the listener free of re-entrant gateway calls).
  std::vector<std::uint64_t> rtt;
  rtt.reserve(opt.orders);
  std::deque<Token> to_cancel;
  std::unordered_map<Token, bool, TokenHash> passive;
  std::size_t acked = 0;
  std::size_t submitted = 0;
  gw.set_listener([&](const OrderUpdate& u) {
    if (u.event != OrderEvent::Accepted) return;
    ++acked;
    const Order& o = *u.order;
    if (o.idx >= opt.warmup) rtt.push_back(o.acked_ns - o.sent_ns);
    if (passive.erase(o.req.token) > 0) to_cancel.push_back(o.req.token);
  });

  gw.connect();
  const auto login_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!gw.active()) {
    gw.poll(1);
    if (std::chrono::steady_clock::now() > login_deadline) {
      std::fprintf(stderr, "login failed: %s\n", gw.last_error().c_str());
      return 1;
    }
  }
  if (opt.fault_rate > 0) {
    sim::FaultConfig f = sim::FaultConfig::uniform(opt.fault_rate, opt.seed);
    f.delay_ms = 1;
    exchange.set_faults(f);
  }

  const std::uint64_t interval_ns = opt.rate > 0 ? static_cast<std::uint64_t>(1e9 / opt.rate) : 0;
  std::uint64_t next_send_ns = now_ns();
  std::uint64_t depth_sum = 0;
  std::uint64_t depth_max = 0;
  std::uint64_t depth_samples = 0;
  std::size_t index = 0;  // workload position, including locally rejected orders

  std::uint64_t t_start = 0;
  std::uint64_t cpu_start = 0;
  std::uint64_t proc_cpu_start = 0;
  Gateway::Stats stats_start{};
  bool timing = false;
  const auto begin_timing = [&] {
    timing = true;
    t_start = now_ns();
    cpu_start = thread_cpu_ns();
    proc_cpu_start = process_cpu_ns();
    stats_start = gw.stats();
  };
  if (opt.warmup == 0) begin_timing();

  const auto settled = [&] {
    return gw.active() && gw.orders().unresolved().empty() &&
           gw.next_seq() - 1 == exchange.sequenced_count(gc.username);
  };
  const auto hard_deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);

  while (submitted < opt.orders || !to_cancel.empty() || !settled()) {
    if (gw.active()) {
      while (!to_cancel.empty()) {
        gw.cancel(to_cancel.front());
        to_cancel.pop_front();
      }
      while (submitted < opt.orders && gw.orders().unresolved().size() < opt.window &&
             (interval_ns == 0 || now_ns() >= next_send_ns)) {
        if (!timing && submitted >= opt.warmup) begin_timing();
        NewOrder o;
        o.stock = sym;
        const std::size_t i = index++;
        if (i % 50 == 25) {
          o.side = 'B';
          o.shares = 5'000;  // above max_order_qty: refused locally
          o.price = kPx;
          gw.submit(o);
          continue;
        }
        const bool is_passive = i % 10 == 9;
        o.side = is_passive ? 'B' : ((i & 1) != 0 ? 'S' : 'B');
        o.shares = 100;
        o.price = is_passive ? kPx - kPriceScale : kPx;
        const SubmitResult r = gw.submit(o);
        if (!r.ok()) break;  // lost the session mid-burst; retry after reconnect
        if (is_passive) passive.emplace(r.token, true);
        ++submitted;
        const std::uint64_t depth = gw.orders().unresolved().size();
        depth_sum += depth;
        depth_max = std::max(depth_max, depth);
        ++depth_samples;
        if (interval_ns != 0) next_send_ns += interval_ns;
      }
    }
    gw.poll(opt.spin ? 0 : 1);
    if (std::chrono::steady_clock::now() > hard_deadline) {
      std::fprintf(stderr, "benchmark did not converge within 10 minutes\n");
      return 1;
    }
  }

  const std::uint64_t wall_ns_total = now_ns() - t_start;
  const std::uint64_t cpu_ns_total = thread_cpu_ns() - cpu_start;
  const std::uint64_t proc_cpu_ns_total = process_cpu_ns() - proc_cpu_start;
  const Gateway::Stats gs = gw.stats();
  const auto& os = gw.orders().stats();
  const auto es = exchange.stats();

  // Final state must agree with the exchange, with or without faults.
  std::size_t mismatches = 0;
  std::unordered_map<Token, sim::OrderTruth, TokenHash> truth;
  for (const auto& t : exchange.truth(gc.username)) truth[t.token] = t;
  for (const Order& o : gw.orders().orders()) {
    auto it = truth.find(o.req.token);
    const std::uint32_t open = it == truth.end() ? 0 : it->second.open;
    const std::uint32_t executed = it == truth.end() ? 0 : it->second.executed;
    if (o.leaves != open || o.cum_qty != executed) ++mismatches;
  }
  const std::string invariants = gw.orders().check_invariants();
  const bool consistent = mismatches == 0 && invariants.empty();

  const double wall_s = static_cast<double>(wall_ns_total) / 1e9;
  const std::uint64_t sent = gs.requests_sent - stats_start.requests_sent;
  const std::uint64_t risk_rejects = gs.risk_rejects - stats_start.risk_rejects;
  const std::size_t timed_orders = opt.orders - opt.warmup;
  bench::Summary s = bench::summarize(rtt);

  std::printf("gateway_bench [%s]\n", opt.label.c_str());
  std::printf(
      "  config        orders=%zu warmup=%zu window=%zu rate=%.0f/s spin=%d nodelay=%d "
      "journal=%s fault_rate=%.3f seed=%llu\n",
      opt.orders, opt.warmup, opt.window, opt.rate, opt.spin ? 1 : 0, opt.nodelay ? 1 : 0,
      opt.journal.c_str(), opt.fault_rate, static_cast<unsigned long long>(opt.seed));
  std::printf("  build         %s, %s\n", bench::compiler_string().c_str(), bench::build_type());
  std::printf("  timed section %.3f s, %zu orders, %llu requests sent (orders + cancels)\n", wall_s,
              timed_orders, static_cast<unsigned long long>(sent));
  std::printf("  throughput    %.0f requests/s, %.0f orders/s, %.0f risk rejects/s\n",
              static_cast<double>(sent) / wall_s, static_cast<double>(timed_orders) / wall_s,
              static_cast<double>(risk_rejects) / wall_s);
  std::printf(
      "  submit->ack   n=%zu min=%.1f p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f max=%.1f "
      "mean=%.1f (us)\n",
      s.count, static_cast<double>(s.min) / 1e3, static_cast<double>(s.p50) / 1e3,
      static_cast<double>(s.p90) / 1e3, static_cast<double>(s.p99) / 1e3,
      static_cast<double>(s.p999) / 1e3, static_cast<double>(s.max) / 1e3, s.mean / 1e3);
  std::printf("  cpu           gateway thread %.1f%% of wall, whole process %.1f%% of one core\n",
              100.0 * static_cast<double>(cpu_ns_total) / static_cast<double>(wall_ns_total),
              100.0 * static_cast<double>(proc_cpu_ns_total) / static_cast<double>(wall_ns_total));
  std::printf(
      "  queue depth   mean %.2f, max %llu awaiting response; max send queue %llu bytes\n",
      depth_samples > 0 ? static_cast<double>(depth_sum) / static_cast<double>(depth_samples) : 0.0,
      static_cast<unsigned long long>(depth_max),
      static_cast<unsigned long long>(gs.max_send_queue_bytes));
  std::printf(
      "  faults        disconnects=%llu duplicates=%llu reorders=%llu delays=%llu "
      "(injected by simulator)\n",
      static_cast<unsigned long long>(es.injected_disconnects),
      static_cast<unsigned long long>(es.injected_duplicates),
      static_cast<unsigned long long>(es.injected_reorders),
      static_cast<unsigned long long>(es.injected_delays));
  std::printf(
      "  recovery      logins=%llu uncertain=%llu resends=%llu duplicates_dropped=%llu "
      "implicit_accepts=%llu anomalies=%llu\n",
      static_cast<unsigned long long>(gs.logins), static_cast<unsigned long long>(os.uncertain),
      static_cast<unsigned long long>(gs.resends), static_cast<unsigned long long>(os.duplicates),
      static_cast<unsigned long long>(os.implicit_accepts),
      static_cast<unsigned long long>(os.anomalies));
  std::printf("  final state   %zu orders, %zu mismatches vs exchange, invariants %s => %s\n",
              gw.orders().orders().size(), mismatches, invariants.empty() ? "ok" : "BROKEN",
              consistent ? "CONSISTENT" : "INCONSISTENT");

  if (!opt.samples.empty()) {
    if (std::FILE* f = std::fopen(opt.samples.c_str(), "w")) {
      std::fprintf(f, "rtt_ns\n");
      for (std::uint64_t v : rtt) std::fprintf(f, "%llu\n", static_cast<unsigned long long>(v));
      std::fclose(f);
    }
  }
  if (!opt.json.empty()) {
    if (std::FILE* f = std::fopen(opt.json.c_str(), "a")) {
      std::fprintf(
          f,
          "{\"label\":\"%s\",\"orders\":%zu,\"warmup\":%zu,\"window\":%zu,\"rate\":%.0f,"
          "\"spin\":%d,\"nodelay\":%d,\"journal\":\"%s\",\"fault_rate\":%.4f,\"seed\":%llu,"
          "\"wall_s\":%.6f,\"requests_sent\":%llu,\"requests_per_s\":%.1f,\"orders_per_s\":%.1f,"
          "\"risk_rejects_per_s\":%.1f,\"rtt_samples\":%zu,\"rtt_min_ns\":%llu,\"rtt_p50_ns\":%llu,"
          "\"rtt_p90_ns\":%llu,\"rtt_p99_ns\":%llu,\"rtt_p999_ns\":%llu,\"rtt_max_ns\":%llu,"
          "\"rtt_mean_ns\":%.1f,\"gateway_cpu_pct\":%.2f,\"process_cpu_pct\":%.2f,"
          "\"queue_depth_mean\":%.3f,\"queue_depth_max\":%llu,\"max_send_queue_bytes\":%llu,"
          "\"injected_disconnects\":%llu,\"injected_duplicates\":%llu,\"injected_reorders\":%llu,"
          "\"injected_delays\":%llu,\"logins\":%llu,\"uncertain\":%llu,\"resends\":%llu,"
          "\"duplicates_dropped\":%llu,\"implicit_accepts\":%llu,\"anomalies\":%llu,"
          "\"mismatches\":%zu,\"consistent\":%s}\n",
          opt.label.c_str(), opt.orders, opt.warmup, opt.window, opt.rate, opt.spin ? 1 : 0,
          opt.nodelay ? 1 : 0, opt.journal.c_str(), opt.fault_rate,
          static_cast<unsigned long long>(opt.seed), wall_s, static_cast<unsigned long long>(sent),
          static_cast<double>(sent) / wall_s, static_cast<double>(timed_orders) / wall_s,
          static_cast<double>(risk_rejects) / wall_s, s.count,
          static_cast<unsigned long long>(s.min), static_cast<unsigned long long>(s.p50),
          static_cast<unsigned long long>(s.p90), static_cast<unsigned long long>(s.p99),
          static_cast<unsigned long long>(s.p999), static_cast<unsigned long long>(s.max), s.mean,
          100.0 * static_cast<double>(cpu_ns_total) / static_cast<double>(wall_ns_total),
          100.0 * static_cast<double>(proc_cpu_ns_total) / static_cast<double>(wall_ns_total),
          depth_samples > 0 ? static_cast<double>(depth_sum) / static_cast<double>(depth_samples)
                            : 0.0,
          static_cast<unsigned long long>(depth_max),
          static_cast<unsigned long long>(gs.max_send_queue_bytes),
          static_cast<unsigned long long>(es.injected_disconnects),
          static_cast<unsigned long long>(es.injected_duplicates),
          static_cast<unsigned long long>(es.injected_reorders),
          static_cast<unsigned long long>(es.injected_delays),
          static_cast<unsigned long long>(gs.logins), static_cast<unsigned long long>(os.uncertain),
          static_cast<unsigned long long>(gs.resends),
          static_cast<unsigned long long>(os.duplicates),
          static_cast<unsigned long long>(os.implicit_accepts),
          static_cast<unsigned long long>(os.anomalies), mismatches, consistent ? "true" : "false");
      std::fclose(f);
    }
  }

  gw.logout();
  exchange.stop();
  if (!journal_path.empty()) std::remove(journal_path.c_str());
  return consistent ? 0 : 1;
}

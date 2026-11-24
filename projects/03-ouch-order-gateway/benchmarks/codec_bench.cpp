// Codec-only latency: encode and decode cost per message, no sockets involved.
//
// Method: for each message type, a pool of 1024 distinct messages is encoded
// (or decoded) in a tight loop of `--iters` operations; the loop is timed as a
// whole and divided by the operation count. The loop is repeated `--reps`
// times and the minimum and median per-operation times are reported. Timing a
// whole loop measures throughput-style cost (the CPU may overlap iterations),
// which is the right figure for "how much does the codec add per message".
//
// Usage: codec_bench [--iters N] [--reps R] [--csv path]
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "bench_util.hpp"
#include "ouch/soupbin.hpp"
#include "ouch/wire.hpp"

using namespace ouch;
using namespace ouch::wire;

namespace {

constexpr std::size_t kPool = 1024;

struct Row {
  std::string name;
  std::size_t bytes;
  double min_ns;
  double median_ns;
};

template <typename Fn>
Row measure(const std::string& name, std::size_t bytes, std::size_t iters, int reps, Fn&& body) {
  std::vector<double> per_op;
  for (int r = 0; r < reps + 1; ++r) {  // first repetition is warm-up
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iters; ++i) body(i & (kPool - 1));
    const auto t1 = std::chrono::steady_clock::now();
    const double ns =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    if (r > 0) per_op.push_back(ns / static_cast<double>(iters));
  }
  std::sort(per_op.begin(), per_op.end());
  return Row{name, bytes, per_op.front(), per_op[per_op.size() / 2]};
}

Token token_for(std::size_t i) {
  char buf[15];
  std::snprintf(buf, sizeof(buf), "BENCH%09zu", i);
  return Token(buf);
}

template <typename M>
using Encoded = std::array<std::uint8_t, M::kSize>;

template <typename M>
std::vector<Encoded<M>> encode_pool(const std::vector<M>& pool) {
  std::vector<Encoded<M>> out(pool.size());
  for (std::size_t i = 0; i < pool.size(); ++i) encode(pool[i], out[i]);
  return out;
}

template <typename M>
void bench_inbound(const char* name, const std::vector<M>& pool, std::size_t iters, int reps,
                   std::vector<Row>& rows) {
  std::array<std::uint8_t, kMaxMessageSize> buf{};
  rows.push_back(measure(std::string("encode ") + name, M::kSize, iters, reps, [&](std::size_t i) {
    bench::do_not_optimize(encode(pool[i], buf));
    bench::do_not_optimize(buf);
  }));
  const auto wire_pool = encode_pool(pool);
  InboundMessage m;
  rows.push_back(measure(std::string("decode ") + name, M::kSize, iters, reps, [&](std::size_t i) {
    bench::do_not_optimize(decode_inbound(wire_pool[i], m));
    bench::do_not_optimize(m);
  }));
}

template <typename M>
void bench_outbound(const char* name, const std::vector<M>& pool, std::size_t iters, int reps,
                    std::vector<Row>& rows) {
  std::array<std::uint8_t, kMaxMessageSize> buf{};
  rows.push_back(measure(std::string("encode ") + name, M::kSize, iters, reps, [&](std::size_t i) {
    bench::do_not_optimize(encode(pool[i], buf));
    bench::do_not_optimize(buf);
  }));
  const auto wire_pool = encode_pool(pool);
  OutboundMessage m;
  rows.push_back(measure(std::string("decode ") + name, M::kSize, iters, reps, [&](std::size_t i) {
    bench::do_not_optimize(decode_outbound(wire_pool[i], m));
    bench::do_not_optimize(m);
  }));
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t iters = 5'000'000;
  int reps = 9;
  std::string csv;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string a = argv[i];
    if (a == "--iters") {
      iters = std::strtoull(argv[i + 1], nullptr, 10);
    } else if (a == "--reps") {
      reps = std::atoi(argv[i + 1]);
    } else if (a == "--csv") {
      csv = argv[i + 1];
    }
  }

  std::vector<EnterOrder> enters(kPool);
  std::vector<ReplaceOrder> replaces(kPool);
  std::vector<CancelOrder> cancels(kPool);
  std::vector<Accepted> accepts(kPool);
  std::vector<Replaced> replaceds(kPool);
  std::vector<Executed> execs(kPool);
  std::vector<Canceled> canceleds(kPool);
  std::vector<Rejected> rejects(kPool);
  for (std::size_t i = 0; i < kPool; ++i) {
    const Token t = token_for(i);
    const auto n = static_cast<std::uint32_t>(i);
    enters[i].token = t;
    enters[i].side = (i & 1) != 0 ? 'B' : 'S';
    enters[i].shares = 100 + n;
    enters[i].stock = Symbol("AAPL");
    enters[i].price = 1'500'000 + n * 100;
    enters[i].firm = Firm("FIRM");
    replaces[i] =
        ReplaceOrder{t, token_for(i + kPool), 200 + n, 1'500'000 + n, kTifMarketHours, 'Y', 'N', 0};
    cancels[i] = CancelOrder{t, n};
    accepts[i].timestamp = 34'200'000'000'000ULL + i;
    accepts[i].token = t;
    accepts[i].shares = 100 + n;
    accepts[i].stock = Symbol("AAPL");
    accepts[i].price = 1'500'000 + n * 100;
    accepts[i].order_ref = 1'000'000 + i;
    replaceds[i].timestamp = accepts[i].timestamp;
    replaceds[i].token = token_for(i + kPool);
    replaceds[i].previous_token = t;
    replaceds[i].shares = 200 + n;
    replaceds[i].order_ref = 2'000'000 + i;
    execs[i] = Executed{accepts[i].timestamp, t, 100 + n, 1'500'000 + n, 'A', 5'000'000 + i};
    canceleds[i] = Canceled{accepts[i].timestamp, t, 100 + n, 'U'};
    rejects[i] = Rejected{accepts[i].timestamp, t, 'S'};
  }

  std::vector<Row> rows;
  bench_inbound("EnterOrder", enters, iters, reps, rows);
  bench_inbound("ReplaceOrder", replaces, iters, reps, rows);
  bench_inbound("CancelOrder", cancels, iters, reps, rows);
  bench_outbound("Accepted", accepts, iters, reps, rows);
  bench_outbound("Replaced", replaceds, iters, reps, rows);
  bench_outbound("Executed", execs, iters, reps, rows);
  bench_outbound("Canceled", canceleds, iters, reps, rows);
  bench_outbound("Rejected", rejects, iters, reps, rows);

  // Receive path as the gateway runs it: SoupBinTCP reassembly + OUCH decode
  // of a stream of sequenced Executed messages, fed in 64 KiB chunks.
  {
    std::vector<std::uint8_t> stream;
    const auto wire_pool = encode_pool(execs);
    for (const auto& e : wire_pool) soup::append_packet(stream, soup::kSequencedData, e);
    soup::FrameParser parser;
    OutboundMessage m;
    const std::size_t rounds = std::max<std::size_t>(1, iters / kPool);
    std::vector<double> per_op;
    for (int r = 0; r < reps + 1; ++r) {
      std::size_t decoded = 0;
      const auto t0 = std::chrono::steady_clock::now();
      for (std::size_t k = 0; k < rounds; ++k) {
        parser.feed(stream);
        char type = 0;
        std::span<const std::uint8_t> payload;
        while (parser.next(type, payload) == soup::FrameParser::Status::Packet) {
          bench::do_not_optimize(decode_outbound(payload, m));
          ++decoded;
        }
      }
      const auto t1 = std::chrono::steady_clock::now();
      bench::do_not_optimize(m);
      const double ns = static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
      if (r > 0) per_op.push_back(ns / static_cast<double>(decoded));
    }
    std::sort(per_op.begin(), per_op.end());
    rows.push_back(Row{"deframe+decode Executed", Executed::kSize + 3, per_op.front(),
                       per_op[per_op.size() / 2]});
  }

  std::printf("codec_bench: %zu ops/loop, %d timed loops, %s, %s\n", iters, reps,
              bench::compiler_string().c_str(), bench::build_type());
  std::printf("%-28s %6s %12s %12s\n", "operation", "bytes", "min ns/op", "median ns/op");
  for (const Row& r : rows) {
    std::printf("%-28s %6zu %12.2f %12.2f\n", r.name.c_str(), r.bytes, r.min_ns, r.median_ns);
  }
  if (!csv.empty()) {
    if (std::FILE* f = std::fopen(csv.c_str(), "w")) {
      std::fprintf(f, "operation,bytes,min_ns_per_op,median_ns_per_op,iters,reps\n");
      for (const Row& r : rows) {
        std::fprintf(f, "%s,%zu,%.3f,%.3f,%zu,%d\n", r.name.c_str(), r.bytes, r.min_ns, r.median_ns,
                     iters, reps);
      }
      std::fclose(f);
    }
  }
  return 0;
}

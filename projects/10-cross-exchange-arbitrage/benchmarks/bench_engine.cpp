#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "xarb/audit.hpp"
#include "xarb/feed_codec.hpp"
#include "xarb/instrument.hpp"
#include "xarb/order_book.hpp"
#include "xarb/replay_engine.hpp"
#include "xarb/synth_feed.hpp"

using namespace xarb;

namespace {

struct OpWorkload {
  bool is_walk{false};
  Side side{Side::Bid};
  PriceTicks price{0};
  QtyLots qty{0};
};

std::vector<OpWorkload> make_book_workload(size_t n_ops, size_t depth, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<OpWorkload> ops;
  ops.reserve(n_ops);
  const PriceTicks mid = 6'500'000;
  std::uniform_int_distribution<size_t> lvl_dist(0, depth - 1);
  std::uniform_int_distribution<QtyLots> qty_dist(10'000, 500'000);

  for (size_t i = 0; i < n_ops; ++i) {
    OpWorkload op;
    if (i % 5 == 0) {
      op.is_walk = true;
      op.side = (rng() % 2 == 0) ? Side::Ask : Side::Bid;
      op.qty = 350'000;
    } else {
      op.is_walk = false;
      op.side = (rng() % 2 == 0) ? Side::Bid : Side::Ask;
      const PriceTicks offset = static_cast<PriceTicks>(lvl_dist(rng) + 1) * 10;
      op.price = (op.side == Side::Bid) ? (mid - offset) : (mid + offset);
      op.qty = (rng() % 8 == 0) ? 0 : qty_dist(rng);
    }
    ops.push_back(op);
  }
  return ops;
}

template <typename BookT>
struct BookBenchResult {
  std::string impl;
  size_t depth{0};
  size_t ops{0};
  double wall_sec{0.0};
  double mops_per_sec{0.0};
  PercentileStats lat_ns{};
  uint64_t sink{0};
};

template <typename BookT>
BookBenchResult<BookT> bench_book_impl(const std::string& label, size_t depth, const std::vector<OpWorkload>& ops,
                                       size_t warmup_ops) {
  BookT book(depth);
  std::vector<PriceLevel> init_b, init_a;
  for (size_t i = 0; i < depth; ++i) {
    init_b.push_back({6'500'000 - static_cast<PriceTicks>(i + 1) * 10, 200'000});
    init_a.push_back({6'500'000 + static_cast<PriceTicks>(i + 1) * 10, 200'000});
  }
  book.apply_snapshot(init_b, init_a);

  volatile uint64_t sink = 0;
  for (size_t i = 0; i < std::min(warmup_ops, ops.size()); ++i) {
    const auto& op = ops[i];
    if (op.is_walk) {
      auto r = (op.side == Side::Ask) ? book.walk_asks(op.qty) : book.walk_bids(op.qty);
      sink += static_cast<uint64_t>(r.filled_lots);
    } else {
      book.apply_delta(op.side, op.price, op.qty);
    }
  }

  // Batch timing for per-op latency distribution (batches of 16 ops to avoid timer resolution floor on arm64/x86).
  constexpr size_t kBatch = 16;
  std::vector<double> per_op_ns;
  per_op_ns.reserve(ops.size() / kBatch + 1);

  const auto total_t0 = std::chrono::steady_clock::now();
  for (size_t i = 0; i + kBatch <= ops.size(); i += kBatch) {
    const auto b0 = std::chrono::steady_clock::now();
    for (size_t k = 0; k < kBatch; ++k) {
      const auto& op = ops[i + k];
      if (op.is_walk) {
        auto r = (op.side == Side::Ask) ? book.walk_asks(op.qty) : book.walk_bids(op.qty);
        sink += static_cast<uint64_t>(r.filled_lots);
      } else {
        book.apply_delta(op.side, op.price, op.qty);
      }
    }
    const auto b1 = std::chrono::steady_clock::now();
    const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(b1 - b0).count()) /
                      static_cast<double>(kBatch);
    per_op_ns.push_back(ns);
  }
  const auto total_t1 = std::chrono::steady_clock::now();

  BookBenchResult<BookT> res;
  res.impl = label;
  res.depth = depth;
  res.ops = ops.size();
  res.wall_sec = std::chrono::duration<double>(total_t1 - total_t0).count();
  res.mops_per_sec = (res.wall_sec > 0.0) ? (static_cast<double>(ops.size()) / res.wall_sec / 1e6) : 0.0;
  res.lat_ns = compute_percentiles(std::move(per_op_ns));
  res.sink = sink;
  return res;
}

}  // namespace

int main(int argc, char** argv) {
  std::string out_dir = "results";
  size_t e2e_steps = 15000;
  size_t book_ops = 400000;

  for (int i = 1; i + 1 < argc; ++i) {
    std::string k = argv[i];
    if (k == "--out-dir") out_dir = argv[i + 1];
    else if (k == "--steps") e2e_steps = static_cast<size_t>(std::stoull(argv[i + 1]));
    else if (k == "--book-ops") book_ops = static_cast<size_t>(std::stoull(argv[i + 1]));
  }

  std::cout << "=== 1. Data-Structure Tradeoff: FlatOrderBook vs MapOrderBook ===\n";
  std::cout << std::left << std::setw(14) << "impl" << std::setw(8) << "depth" << std::setw(12) << "Mops/s"
            << std::setw(10) << "p50_ns" << std::setw(10) << "p99_ns" << std::setw(10) << "p999_ns" << "\n";

  std::ostringstream book_csv;
  book_csv << "impl,depth,ops,wall_sec,mops_per_sec,p50_ns,p95_ns,p99_ns,p999_ns\n";

  for (size_t depth : {20UL, 50UL, 200UL}) {
    auto ops = make_book_workload(book_ops, depth, 1000 + depth);
    auto r_flat = bench_book_impl<FlatOrderBook>("FlatOrderBook", depth, ops, 20000);
    auto r_map = bench_book_impl<MapOrderBook>("MapOrderBook", depth, ops, 20000);

    for (const auto& [impl, d, o, w, mops, lat] :
         {std::make_tuple(r_flat.impl, r_flat.depth, r_flat.ops, r_flat.wall_sec, r_flat.mops_per_sec, r_flat.lat_ns),
          std::make_tuple(r_map.impl, r_map.depth, r_map.ops, r_map.wall_sec, r_map.mops_per_sec, r_map.lat_ns)}) {
      std::cout << std::fixed << std::setprecision(2) << std::left << std::setw(14) << impl << std::setw(8) << d
                << std::setw(12) << mops << std::setprecision(1) << std::setw(10) << lat.p50 << std::setw(10)
                << lat.p99 << std::setw(10) << lat.p999 << "\n";
      book_csv << impl << ',' << d << ',' << o << ',' << std::setprecision(6) << w << ',' << std::setprecision(3)
               << mops << ',' << lat.p50 << ',' << lat.p95 << ',' << lat.p99 << ',' << lat.p999 << "\n";
    }
  }

  std::cout << "\n=== 2. End-to-End 3-Venue Pipeline Benchmark (JSON parse + CRC32 + L2 + VWAP Scan + Paper Sim) ===\n";
  auto reg = InstrumentRegistry::make_default_crypto_registry();
  SynthFeedConfig scfg;
  scfg.num_steps = e2e_steps;
  scfg.seed = 20261124ULL;
  scfg.inject_faults = true;
  auto feed = generate_synthetic_feed(reg, scfg);

  // Warmup pass on first 2000 events
  {
    std::vector<RecordedFeedEvent> warm(
        feed.begin(), feed.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(2000, feed.size())));
    ReplayConfig wcfg;
    wcfg.measure_processing_latency = false;
    ReplayEngine we(reg, wcfg);
    (void)we.run(warm, "warmup");
  }

  ReplayConfig rcfg;
  rcfg.scenario_name = "e2e_benchmark";
  rcfg.measure_processing_latency = true;
  ReplayEngine engine(reg, rcfg);
  auto res = engine.run(feed, "synthetic_e2e");
  std::cout << format_summary_text(res.summary);

  // 3. Binary vs JSONL decode throughput
  const std::string bin_path = out_dir + "/bench_feed.xarb";
  const std::string jsonl_path = out_dir + "/bench_feed.jsonl";
  write_binary_feed(bin_path, feed);
  write_jsonl_feed(jsonl_path, feed);

  std::vector<RecordedFeedEvent> decoded;
  auto tb0 = std::chrono::steady_clock::now();
  read_binary_feed(bin_path, decoded);
  auto tb1 = std::chrono::steady_clock::now();
  const double bin_sec = std::chrono::duration<double>(tb1 - tb0).count();

  auto tj0 = std::chrono::steady_clock::now();
  read_jsonl_feed(jsonl_path, decoded);
  auto tj1 = std::chrono::steady_clock::now();
  const double jsonl_sec = std::chrono::duration<double>(tj1 - tj0).count();

  std::cout << "\n=== 3. Feed Codec Throughput (" << feed.size() << " events) ===\n";
  std::cout << std::fixed << std::setprecision(1)
            << "Binary (CRC32): " << (static_cast<double>(feed.size()) / bin_sec) << " events/s ("
            << std::setprecision(4) << bin_sec << " s)\n"
            << "JSONL:          " << std::setprecision(1) << (static_cast<double>(feed.size()) / jsonl_sec)
            << " events/s (" << std::setprecision(4) << jsonl_sec << " s)\n";

  // Write raw benchmark CSVs
  {
    std::ofstream ofs(out_dir + "/book_tradeoff.csv", std::ios::trunc);
    ofs << book_csv.str();
  }
  {
    std::ofstream ofs(out_dir + "/e2e_benchmark.json", std::ios::trunc);
    ofs << std::fixed << std::setprecision(4) << "{\n"
        << "  \"events\": " << res.summary.total_events << ",\n"
        << "  \"wall_time_sec\": " << res.summary.wall_time_sec << ",\n"
        << "  \"events_per_sec\": " << res.summary.events_per_sec << ",\n"
        << "  \"book_updates_per_sec\": " << res.summary.book_updates_per_sec << ",\n"
        << "  \"proc_p50_ns\": " << res.summary.processing_latency_ns.p50 << ",\n"
        << "  \"proc_p99_ns\": " << res.summary.processing_latency_ns.p99 << ",\n"
        << "  \"proc_p999_ns\": " << res.summary.processing_latency_ns.p999 << ",\n"
        << "  \"binary_decode_events_per_sec\": " << (static_cast<double>(feed.size()) / bin_sec) << ",\n"
        << "  \"jsonl_decode_events_per_sec\": " << (static_cast<double>(feed.size()) / jsonl_sec) << "\n"
        << "}\n";
  }

  return 0;
}

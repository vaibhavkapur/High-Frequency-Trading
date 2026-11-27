#include <filesystem>
#include <fstream>
#include <sstream>

#include "hft/runtime/pipeline.hpp"
#include "test_framework.hpp"

using namespace hft::contracts::v1;
using namespace hft::adapters;
using namespace hft::runtime;

HFT_TEST(DeterministicSingleVenueReplayIsBitIdenticalAcrossRunsAndBackends) {
  const std::vector<InstrumentMeta> insts = {InstrumentMeta::make(0, "AAPL", 100, 1)};
  VenueMeta venue{};
  venue.id = 0;
  venue.name = "SIM_XNAS";
  venue.maker_fee_bps_x100 = -20;
  venue.taker_fee_bps_x100 = 30;
  venue.md_latency_ns = 50'000;
  venue.order_latency_ns = 80'000;
  venue.response_latency_ns = 60'000;

  const auto feed = DeterministicEnginePipeline::make_golden_single_venue_feed(0, 0);
  DeterministicEnginePipeline pipe;

  auto run_once = [&](BookBackend b) {
    ScenarioConfig cfg{};
    cfg.scenario_name = "single-venue";
    cfg.book_backend = b;
    cfg.seed = 42;
    cfg.risk.max_order_qty_lots = 200;
    cfg.risk.max_long_position_lots = 300;
    cfg.risk.max_short_position_lots = 300;
    SimpleDeterministicStrategy strat;
    return pipe.run_single_venue(cfg, feed, strat, insts, venue);
  };

  const auto r1 = run_once(BookBackend::Pool);
  const auto r2 = run_once(BookBackend::Pool);
  const auto r_map = run_once(BookBackend::Map);

  HFT_EXPECT_TRUE(r1.all_invariants_ok());
  HFT_EXPECT_EQ(r1.causality_violations, 0ULL);
  HFT_EXPECT_EQ(r1.ledger_discrepancy_scaled, 0LL);

  // Must exercise accepted, fills, canceled, and risk-rejected paths in MVP!
  HFT_EXPECT_TRUE(r1.orders_accepted > 0);
  HFT_EXPECT_TRUE(r1.fills_executed > 0);
  HFT_EXPECT_TRUE(r1.orders_canceled > 0);
  HFT_EXPECT_TRUE(r1.risk_rejected > 0);

  // Bit-identical replay checksum across repeated runs AND across PoolBook vs MapBook!
  HFT_EXPECT_EQ(r1.replay_checksum, r2.replay_checksum);
  HFT_EXPECT_EQ(r1.final_book_hash, r2.final_book_hash);
  HFT_EXPECT_EQ(r1.replay_checksum, r_map.replay_checksum);
  HFT_EXPECT_EQ(r1.final_book_hash, r_map.final_book_hash);
  HFT_EXPECT_EQ(r1.cash_scaled, r_map.cash_scaled);
  HFT_EXPECT_EQ(r1.final_position_lots, r_map.final_position_lots);

  // Check against golden fixture file if present.
  const std::string golden_path = std::string(HFT_GOLDEN_DIR) + "/single_venue.golden.json";
  if (std::filesystem::exists(golden_path)) {
    std::ifstream ifs(golden_path);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    HFT_EXPECT_EQ(r1.to_json(), oss.str());
  }
}

HFT_TEST(ItchStreamReconstructsReferenceBookBboAndExecutesCausally) {
  const std::vector<InstrumentMeta> insts = {InstrumentMeta::make(0, "S00001", 100, 1)};
  VenueMeta venue{};
  venue.id = 0;
  venue.name = "ITCH_XNAS";

  const auto raw_itch = ItchFeedAdapter::generate_synthetic_stream(1200, 1, 99);
  ItchFeedAdapter itch_ad(0, insts);
  std::vector<Envelope> feed;
  HFT_EXPECT_EQ(itch_ad.decode_stream(raw_itch, feed), ErrorCode::Ok);
  HFT_EXPECT_TRUE(feed.size() > 200);

  CanonicalOrderBook canon_book(1'000'000'000'000LL);
  for (const auto& env : feed) {
    canon_book.apply(std::get<MarketDataEvent>(env.payload), env.event_time_ns);
  }
  const TopOfBook ref_tob = itch_ad.reference_tob(1);
  const TopOfBook got_tob = canon_book.top_of_book(SimTimeNs{100'000'000'000'000LL});
  HFT_EXPECT_EQ(ref_tob.bid.has_value(), got_tob.bid.has_value());
  HFT_EXPECT_EQ(ref_tob.ask.has_value(), got_tob.ask.has_value());
  if (ref_tob.bid.has_value()) {
    HFT_EXPECT_EQ(ref_tob.bid->price, got_tob.bid->price);
    HFT_EXPECT_EQ(ref_tob.bid->qty, got_tob.bid->qty);
  }
  if (ref_tob.ask.has_value()) {
    HFT_EXPECT_EQ(ref_tob.ask->price, got_tob.ask->price);
    HFT_EXPECT_EQ(ref_tob.ask->qty, got_tob.ask->qty);
  }

  ScenarioConfig cfg{};
  cfg.scenario_name = "itch-e2e";
  cfg.risk.max_order_qty_lots = 500;
  cfg.risk.max_order_notional = Money{10'000'000LL * kMoneyScale};
  cfg.risk.max_long_position_lots = 2000;
  cfg.risk.max_short_position_lots = 2000;
  SimpleDeterministicStrategy strat;
  DeterministicEnginePipeline pipe;
  const auto res = pipe.run_single_venue(cfg, feed, strat, insts, venue);
  HFT_EXPECT_TRUE(res.all_invariants_ok());
  HFT_EXPECT_EQ(res.causality_violations, 0ULL);
}

int main() { return hft::test::run_all("test_deterministic_e2e"); }

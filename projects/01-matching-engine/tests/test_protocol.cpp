// Wire-format, text-format, replay-determinism and binary-input fuzz tests.
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <sstream>

#include "me/workload.hpp"
#include "reference_engine.hpp"
#include "test_util.hpp"

namespace me::test {
namespace {

std::uint64_t splitmix(std::uint64_t& state) {
  std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

// Replays commands through a fresh engine and returns the raw report byte stream.
template <class Book>
std::vector<std::byte> replay_bytes(const EngineConfig& config, const std::vector<Command>& commands) {
  Engine<Book> engine(config);
  std::vector<std::byte> bytes;
  for (const Command& c : commands) {
    engine.process(c, [&bytes](const Report& r) {
      std::array<std::byte, kReportSize> buf;
      encode_report(r, buf);
      bytes.insert(bytes.end(), buf.begin(), buf.end());
    });
  }
  return bytes;
}

TEST(Protocol, LayoutMatchesDocumentedOffsets) {
  EXPECT_EQ(sizeof(Command), 40u);
  EXPECT_EQ(offsetof(Command, type), 0u);
  EXPECT_EQ(offsetof(Command, side), 1u);
  EXPECT_EQ(offsetof(Command, reserved), 2u);
  EXPECT_EQ(offsetof(Command, symbol), 4u);
  EXPECT_EQ(offsetof(Command, order_id), 8u);
  EXPECT_EQ(offsetof(Command, new_order_id), 16u);
  EXPECT_EQ(offsetof(Command, price), 24u);
  EXPECT_EQ(offsetof(Command, qty), 32u);

  EXPECT_EQ(sizeof(Report), 64u);
  EXPECT_EQ(offsetof(Report, seq), 0u);
  EXPECT_EQ(offsetof(Report, order_id), 8u);
  EXPECT_EQ(offsetof(Report, other_id), 16u);
  EXPECT_EQ(offsetof(Report, price), 24u);
  EXPECT_EQ(offsetof(Report, qty), 32u);
  EXPECT_EQ(offsetof(Report, leaves), 40u);
  EXPECT_EQ(offsetof(Report, other_leaves), 48u);
  EXPECT_EQ(offsetof(Report, symbol), 56u);
  EXPECT_EQ(offsetof(Report, type), 60u);
  EXPECT_EQ(offsetof(Report, side), 61u);
  EXPECT_EQ(offsetof(Report, reason), 62u);
  EXPECT_EQ(offsetof(Report, reserved), 63u);
}

TEST(Protocol, CommandEncodesToExactLittleEndianBytes) {
  const Command c = new_order(0x01020304, 0x1112131415161718ull, Side::Sell, 0x2122232425262728ll, 258);
  std::array<std::byte, kCommandSize> buf;
  encode_command(c, buf);
  const unsigned char expected[kCommandSize] = {
      0x01, 0x01, 0x00, 0x00,                          // type=New, side=Sell, reserved
      0x04, 0x03, 0x02, 0x01,                          // symbol
      0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,  // order_id
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // new_order_id
      0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,  // price
      0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // qty
  };
  EXPECT_EQ(std::memcmp(buf.data(), expected, kCommandSize), 0);
  EXPECT_EQ(decode_command(buf), c);
}

TEST(Protocol, ReportRoundTrips) {
  Report r;
  r.seq = 42;
  r.order_id = 7;
  r.other_id = 9;
  r.price = -5;
  r.qty = 100;
  r.leaves = 60;
  r.other_leaves = 1;
  r.symbol = 3;
  r.type = ReportType::Trade;
  r.side = Side::Sell;
  r.reason = RejectReason::BookFull;
  std::array<std::byte, kReportSize> buf;
  encode_report(r, buf);
  EXPECT_EQ(decode_report(buf), r);
}

TEST(TextFormat, ParsesEveryCommandKind) {
  Command c;
  ASSERT_EQ(parse_command("N 2 17 B 10050 300", c), ParseStatus::Ok);
  EXPECT_EQ(c, new_order(2, 17, Side::Buy, 10050, 300));
  ASSERT_EQ(parse_command("  N 0 1 S 5 -3  # negative qty is parseable; the engine rejects it", c), ParseStatus::Ok);
  EXPECT_EQ(c, new_order(0, 1, Side::Sell, 5, -3));
  ASSERT_EQ(parse_command("C 1 17", c), ParseStatus::Ok);
  EXPECT_EQ(c, cancel(17, 1));
  ASSERT_EQ(parse_command("R 0 17 40\r", c), ParseStatus::Ok);
  EXPECT_EQ(c, reduce(17, 40));
  ASSERT_EQ(parse_command("X\t0 17 18 10100 50", c), ParseStatus::Ok);
  EXPECT_EQ(c, replace(17, 18, 10100, 50));
}

TEST(TextFormat, BlankLinesAndCommentsAreSkipped) {
  Command c;
  EXPECT_EQ(parse_command("", c), ParseStatus::Blank);
  EXPECT_EQ(parse_command("   \t ", c), ParseStatus::Blank);
  EXPECT_EQ(parse_command("# N 0 1 B 1 1", c), ParseStatus::Blank);
}

TEST(TextFormat, RejectsMalformedLines) {
  Command c;
  std::string error;
  for (const char* line : {"Q 0 1", "N 0 1 B 100", "N 0 1 B 100 10 extra", "N 0 1 Z 100 10", "N x 1 B 100 10",
                           "N 0 -1 B 100 10", "N 0 1 B 1e3 10", "N 0 1 B 100 10.5", "C 0", "R 0 1", "X 0 1 2 100",
                           "NEW 0 1 B 100 10", "N 0 99999999999999999999 B 100 10"}) {
    error.clear();
    EXPECT_EQ(parse_command(line, c, &error), ParseStatus::Error) << line;
    EXPECT_FALSE(error.empty()) << line;
  }
}

TEST(TextFormat, FormatCommandRoundTripsThroughParse) {
  Workload workload(WorkloadConfig{.seed = 9, .symbols = 3});
  for (int i = 0; i < 2000; ++i) {
    const Command c = workload.next();
    Command parsed;
    ASSERT_EQ(parse_command(format_command(c), parsed), ParseStatus::Ok) << format_command(c);
    ASSERT_EQ(parsed, c);
  }
}

TEST(Workload, SameSeedSameStreamDifferentSeedDifferentStream) {
  WorkloadConfig wc;
  wc.symbols = 3;
  wc.invalid_pm = 50;
  const auto a = Workload(wc).generate(5000);
  const auto b = Workload(wc).generate(5000);
  EXPECT_EQ(a, b);
  wc.seed = 2;
  EXPECT_NE(Workload(wc).generate(5000), a);
}

TEST(Workload, RejectsInvalidConfiguration) {
  WorkloadConfig wc;
  wc.add_pm = 999;
  EXPECT_THROW(Workload{wc}, std::invalid_argument);
  wc = WorkloadConfig{};
  wc.symbols = 0;
  EXPECT_THROW(Workload{wc}, std::invalid_argument);
  wc = WorkloadConfig{};
  wc.mid_ticks = 10;
  EXPECT_THROW(Workload{wc}, std::invalid_argument);
}

TEST(Replay, FixedCommandLogProducesByteIdenticalReports) {
  WorkloadConfig wc;
  wc.symbols = 4;
  wc.tick_size = 5;
  wc.lot_size = 10;
  wc.target_live = 2000;
  wc.invalid_pm = 30;
  wc.random_ids = true;
  const std::vector<Command> commands = Workload(wc).generate(100'000);
  const EngineConfig config = make_config(4, 5, 10, 1u << 14);

  const std::vector<std::byte> first = replay_bytes<PoolBook>(config, commands);
  const std::vector<std::byte> second = replay_bytes<PoolBook>(config, commands);
  ASSERT_GT(first.size(), commands.size() * kReportSize);  // trades add reports beyond one per command
  EXPECT_TRUE(first == second) << "two replays of the same log differ";
  // The data structure behind the book is not observable in the output.
  EXPECT_TRUE(replay_bytes<MapBook>(config, commands) == first) << "MapBook and PoolBook streams differ";
}

// Golden-file test: the checked-in sample session must keep producing exactly
// the checked-in report text. The expected file was reviewed by hand.
std::vector<std::string> read_lines(const std::string& path) {
  std::ifstream in(path);
  EXPECT_TRUE(in.good()) << "cannot open " << path;
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) lines.push_back(line);
  return lines;
}

template <class Book>
class GoldenReplay : public ::testing::Test {};
TYPED_TEST_SUITE(GoldenReplay, BookTypes);

TYPED_TEST(GoldenReplay, SampleSessionMatchesExpectedReports) {
  const std::string dir = ME_EXAMPLES_DIR;
  EngineConfig config = make_config(2, 5, 10);
  Engine<TypeParam> engine(config);
  std::vector<std::string> actual;
  for (const std::string& line : read_lines(dir + "/sample_session.txt")) {
    Command c;
    std::string error;
    const ParseStatus st = parse_command(line, c, &error);
    ASSERT_NE(st, ParseStatus::Error) << line << ": " << error;
    if (st == ParseStatus::Ok) {
      engine.process(c, [&actual](const Report& r) { actual.push_back(format_report(r)); });
    }
  }
  EXPECT_EQ(actual, read_lines(dir + "/sample_session.expected.txt"));
}

// Structured fuzzing of the binary input path: random bytes, with a mask applied
// so that a useful share of commands gets past validation. No input may crash the
// engine, break an invariant, or make it disagree with the reference engine.
TEST(Fuzz, RandomBinaryCommandsNeverBreakTheEngine) {
  const EngineConfig config = make_config(2, 2, 3, 16);
  for (std::uint64_t seed = 1; seed <= 4; ++seed) {
    Engine<MapBook> map_engine(config);
    Engine<PoolBook> pool_engine(config);
    ReferenceEngine reference(config);
    std::uint64_t state = seed;
    std::uint64_t accepted = 0;
    for (int step = 0; step < 60'000; ++step) {
      std::array<std::byte, kCommandSize> raw;
      for (std::size_t i = 0; i < raw.size(); i += 8) {
        const std::uint64_t word = splitmix(state);
        std::memcpy(raw.data() + i, &word, 8);
      }
      Command c = decode_command(raw);
      if (splitmix(state) % 8 != 0) {  // 7 in 8 commands are narrowed towards plausible values
        c.type = static_cast<CommandType>(static_cast<unsigned>(c.type) % 6);
        c.side = static_cast<Side>(static_cast<unsigned>(c.side) % 3);
        c.symbol %= 3;
        c.order_id %= 40;
        c.new_order_id %= 40;
        // Mostly on-tick (2) prices in [-2, 14] and on-lot (3) quantities in [-3, 30],
        // with an occasional off-by-one to hit the tick and lot checks.
        const auto p = static_cast<std::uint64_t>(c.price);
        const auto q = static_cast<std::uint64_t>(c.qty);
        c.price = 2 * static_cast<Price>(p % 9) - 2 + ((p >> 32) % 16 == 0 ? 1 : 0);
        c.qty = 3 * static_cast<Qty>(q % 12) - 3 + ((q >> 32) % 16 == 0 ? 1 : 0);
      }
      const std::vector<Report> expected = reference.process(c);
      std::vector<Report> map_out;
      std::vector<Report> pool_out;
      map_engine.process(c, [&map_out](const Report& r) { map_out.push_back(r); });
      pool_engine.process(c, [&pool_out](const Report& r) { pool_out.push_back(r); });
      ASSERT_EQ(map_out, expected) << "seed " << seed << " step " << step;
      ASSERT_EQ(pool_out, expected) << "seed " << seed << " step " << step;
      ASSERT_FALSE(expected.empty());
      accepted += expected[0].type != ReportType::Rejected ? 1 : 0;
      if (step % 256 == 0) {
        for (SymbolId s = 0; s < 2; ++s) {
          std::string why;
          ASSERT_TRUE(map_engine.book(s).check_invariants(&why)) << why;
          ASSERT_TRUE(pool_engine.book(s).check_invariants(&why)) << why;
        }
      }
    }
    EXPECT_GT(accepted, 3000u) << "fuzz mask lets too few commands through to be useful";
  }
}

}  // namespace
}  // namespace me::test

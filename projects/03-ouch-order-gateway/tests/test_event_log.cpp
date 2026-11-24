#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <vector>

#include "ouch/event_log.hpp"
#include "test_util.hpp"

using namespace ouch;
using namespace ouch::wire;
using testutil::TempFile;

namespace {

std::vector<std::uint8_t> bytes_of(const InboundMessage& m) {
  std::vector<std::uint8_t> b(kMaxMessageSize);
  b.resize(encode(m, b));
  return b;
}
std::vector<std::uint8_t> bytes_of(const OutboundMessage& m) {
  std::vector<std::uint8_t> b(kMaxMessageSize);
  b.resize(encode(m, b));
  return b;
}

EnterOrder enter(const char* token, char side, std::uint32_t shares) {
  EnterOrder e;
  e.token = Token(token);
  e.side = side;
  e.shares = shares;
  e.stock = Symbol("AAPL");
  e.price = 1'500'000;
  return e;
}

// Writes each event to the journal and applies it to the live OrderManager,
// the way the gateway does.
struct Recorder {
  EventLog log;
  OrderManager oms;
  std::uint64_t ts = 100;
  std::uint64_t seq = 1;

  void request(const InboundMessage& m) {
    log.append(RecordKind::RequestSent, ++ts, 0, bytes_of(m));
    ASSERT_TRUE(oms.on_request_sent(m, ts));
  }
  void exchange(const OutboundMessage& m) {
    log.append(RecordKind::ExchangeMsg, ++ts, seq++, bytes_of(m));
    oms.on_exchange(m, ts);
  }
  void down() {
    log.append(RecordKind::SessionDown, ++ts, seq);
    oms.on_disconnect();
  }
};

}  // namespace

TEST(EventLog, RoundTripsRecords) {
  TempFile f("roundtrip");
  {
    EventLog log;
    ASSERT_TRUE(log.open(f.path(), false));
    const std::vector<std::uint8_t> payload = {1, 2, 3};
    log.append(RecordKind::RequestSent, 111, 0, payload);
    log.append(RecordKind::ExchangeMsg, 222, 7, {});
    log.append(RecordKind::SessionDown, 333, 8);
    EXPECT_EQ(log.records_written(), 4u);  // includes the Start record
  }
  std::vector<Record> recs;
  ASSERT_TRUE(EventLog::read_all(f.path(), recs));
  ASSERT_EQ(recs.size(), 4u);
  EXPECT_EQ(recs[0].kind, RecordKind::Start);
  EXPECT_EQ(recs[0].payload.size(), 8u);
  EXPECT_EQ(recs[1].kind, RecordKind::RequestSent);
  EXPECT_EQ(recs[1].ts_ns, 111u);
  EXPECT_EQ(recs[1].payload, (std::vector<std::uint8_t>{1, 2, 3}));
  EXPECT_EQ(recs[2].kind, RecordKind::ExchangeMsg);
  EXPECT_EQ(recs[2].seq, 7u);
  EXPECT_TRUE(recs[2].payload.empty());
  EXPECT_EQ(recs[3].kind, RecordKind::SessionDown);
  EXPECT_EQ(recs[3].seq, 8u);
}

TEST(EventLog, IsAppendOnlyAcrossReopen) {
  TempFile f("append");
  for (int run = 0; run < 3; ++run) {
    EventLog log;
    ASSERT_TRUE(log.open(f.path(), true));
    log.append(RecordKind::SessionDown, static_cast<std::uint64_t>(run), 0);
  }
  std::vector<Record> recs;
  ASSERT_TRUE(EventLog::read_all(f.path(), recs));
  ASSERT_EQ(recs.size(), 6u);  // (Start + SessionDown) x 3, earlier runs intact
  EXPECT_EQ(recs[1].ts_ns, 0u);
  EXPECT_EQ(recs[3].ts_ns, 1u);
  EXPECT_EQ(recs[5].ts_ns, 2u);
}

TEST(EventLog, MissingFileIsReportedAndTornTailIsIgnored) {
  std::vector<Record> recs;
  EXPECT_FALSE(EventLog::read_all("/nonexistent/dir/journal", recs));

  TempFile f("torn");
  {
    EventLog log;
    ASSERT_TRUE(log.open(f.path(), false));
    const std::vector<std::uint8_t> payload(40, 0xAB);
    log.append(RecordKind::RequestSent, 1, 0, payload);
    log.append(RecordKind::RequestSent, 2, 0, payload);
  }
  // Chop the file in the middle of the last record, as a crash would.
  std::ifstream in(f.path(), std::ios::binary);
  std::vector<char> all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  all.resize(all.size() - 17);
  std::ofstream out(f.path(), std::ios::binary | std::ios::trunc);
  out.write(all.data(), static_cast<std::streamsize>(all.size()));
  out.close();

  ASSERT_TRUE(EventLog::read_all(f.path(), recs));
  ASSERT_EQ(recs.size(), 2u);  // Start + the one intact request
  EXPECT_EQ(recs[1].ts_ns, 1u);
}

TEST(EventLog, ReplayRebuildsIdenticalOrderState) {
  TempFile f("replay");
  Recorder r;
  ASSERT_TRUE(r.log.open(f.path(), false));

  const auto a = enter("A", 'B', 100);
  const auto b = enter("B", 'S', 100);
  const auto c = enter("C", 'B', 50);
  Accepted acc_a;
  acc_a.token = a.token;
  acc_a.shares = 100;
  acc_a.price = a.price;
  acc_a.order_ref = 1;
  Accepted acc_b = acc_a;
  acc_b.token = b.token;
  acc_b.side = 'S';
  acc_b.order_ref = 2;

  r.request(a);
  r.exchange(acc_a);
  r.request(b);
  r.exchange(acc_b);
  r.exchange(Executed{0, a.token, 60, 1'500'000, 'A', 1});
  r.exchange(Executed{0, b.token, 60, 1'500'000, 'R', 1});
  r.exchange(Executed{0, b.token, 60, 1'500'000, 'R', 1});  // duplicate
  r.request(ReplaceOrder{a.token, Token("A2"), 120, 1'490'000, kTifMarketHours, 'Y', 'N', 0});
  Replaced rep;
  rep.token = Token("A2");
  rep.previous_token = a.token;
  rep.shares = 60;
  rep.price = 1'490'000;
  rep.order_ref = 3;
  r.exchange(rep);
  r.request(CancelOrder{b.token, 0});
  r.request(c);  // in flight when the session drops
  r.down();
  r.log.close();

  std::vector<Record> recs;
  ASSERT_TRUE(EventLog::read_all(f.path(), recs));
  OrderManager rebuilt;
  const ReplayResult res = replay(recs, rebuilt);

  EXPECT_EQ(res.skipped, 0u);
  EXPECT_EQ(res.next_seq, r.seq);
  EXPECT_EQ(testutil::diff(r.oms, rebuilt), "");
  EXPECT_EQ(rebuilt.stats(), r.oms.stats());
  EXPECT_EQ(rebuilt.exposure(Symbol("AAPL")), r.oms.exposure(Symbol("AAPL")));
  EXPECT_EQ(rebuilt.check_invariants(), "");
  EXPECT_EQ(rebuilt.find(Token("C"))->state, OrderState::Reconciling);
  EXPECT_TRUE(rebuilt.find(b.token)->pending_uncertain);
}

TEST(EventLog, ReplaySkipsUndecodableRecordsButKeepsSequence) {
  std::vector<Record> recs;
  Record junk;
  junk.kind = RecordKind::ExchangeMsg;
  junk.seq = 5;
  junk.payload = {'?', 1, 2};
  recs.push_back(junk);
  junk.kind = RecordKind::RequestSent;
  recs.push_back(junk);
  OrderManager oms;
  const ReplayResult res = replay(recs, oms);
  EXPECT_EQ(res.skipped, 2u);
  EXPECT_EQ(res.next_seq, 6u);
  EXPECT_TRUE(oms.orders().empty());
}

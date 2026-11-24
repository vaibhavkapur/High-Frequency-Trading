// itch_replay - replay a length-prefixed ITCH 5.0 file through the decoder and
// book builder, print a summary, and optionally write CSV outputs.
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "itch/feed_handler.hpp"
#include "itch/framing.hpp"
#include "itch/mapped_file.hpp"
#include "itch/replay.hpp"

namespace {

struct Options {
  std::string file;
  std::string bbo_csv;
  std::string trades_csv;
  std::string snapshot_csv;
  std::string counts_csv;
  std::string summary_csv;
  std::size_t depth = 5;
  std::size_t show = 5;
  double rate = 0.0;   // messages per second; 0 = unlimited
  double speed = 0.0;  // ITCH-time multiplier; 0 = off
  std::uint64_t limit = 0;
};

void usage() {
  std::fputs(
      "usage: itch_replay FILE [options]\n"
      "  --bbo-csv PATH       best bid/offer, one row per change\n"
      "  --trades-csv PATH    one row per E/C/P/Q trade report\n"
      "  --snapshot-csv PATH  final top-of-book levels per symbol\n"
      "  --summary-csv PATH   final BBO, spread, imbalance, microprice, volumes\n"
      "  --counts-csv PATH    decoded message counts by type\n"
      "  --depth N            levels per side in the snapshot (default 5)\n"
      "  --show N             symbols to print in the terminal summary (default 5)\n"
      "  --rate N             throttle to N messages/second\n"
      "  --speed X            pace by ITCH timestamps at X times real time\n"
      "  --limit N            stop after N messages\n",
      stderr);
}

bool parse(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto value = [&]() -> const char* {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--bbo-csv") {
      o.bbo_csv = value();
    } else if (arg == "--trades-csv") {
      o.trades_csv = value();
    } else if (arg == "--snapshot-csv") {
      o.snapshot_csv = value();
    } else if (arg == "--summary-csv") {
      o.summary_csv = value();
    } else if (arg == "--counts-csv") {
      o.counts_csv = value();
    } else if (arg == "--depth") {
      o.depth = std::strtoull(value(), nullptr, 10);
    } else if (arg == "--show") {
      o.show = std::strtoull(value(), nullptr, 10);
    } else if (arg == "--rate") {
      o.rate = std::strtod(value(), nullptr);
    } else if (arg == "--speed") {
      o.speed = std::strtod(value(), nullptr);
    } else if (arg == "--limit") {
      o.limit = std::strtoull(value(), nullptr, 10);
    } else if (!arg.empty() && arg[0] != '-' && o.file.empty()) {
      o.file = arg;
    } else {
      return false;
    }
  }
  return !o.file.empty();
}

std::FILE* open_out(const std::string& path) {
  if (path.empty()) return nullptr;
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (f == nullptr) {
    std::fprintf(stderr, "itch_replay: cannot write %s\n", path.c_str());
    std::exit(1);
  }
  return f;
}

// Price4 as a decimal with exactly four fractional digits.
void put_price(std::FILE* f, itch::Price4 p) { std::fprintf(f, "%u.%04u", p / 10000, p % 10000); }

void put_symbol(std::FILE* f, const itch::OrderBook* b) {
  if (b != nullptr && b->has_symbol) {
    const std::string_view s = b->symbol.view();
    std::fwrite(s.data(), 1, s.size(), f);
  }
}

const char* frame_status_name(itch::FrameStatus s) {
  switch (s) {
    case itch::FrameStatus::Ok:
      return "ok";
    case itch::FrameStatus::End:
      return "end";
    case itch::FrameStatus::ZeroLength:
      return "zero_length";
    case itch::FrameStatus::TruncatedLength:
      return "truncated_length";
    case itch::FrameStatus::TruncatedPayload:
      return "truncated_payload";
  }
  return "?";
}

int run(const Options& opt) {
  const itch::MappedFile file(opt.file);
  itch::LengthPrefixedReader reader(file.bytes());
  itch::FeedHandler<> handler;
  itch::Pacer pacer = opt.speed > 0.0  ? itch::Pacer::time_scaled(opt.speed)
                      : opt.rate > 0.0 ? itch::Pacer::fixed_rate(opt.rate)
                                       : itch::Pacer::unlimited();

  std::FILE* bbo_out = open_out(opt.bbo_csv);
  std::FILE* trades_out = open_out(opt.trades_csv);
  if (bbo_out != nullptr)
    std::fputs("timestamp,locate,symbol,bid_px,bid_qty,ask_px,ask_qty\n", bbo_out);
  if (trades_out != nullptr) {
    std::fputs("timestamp,locate,symbol,kind,price,qty,match,aggressor,printable\n", trades_out);
  }

  std::vector<itch::Bbo> last_bbo;
  std::uint64_t bbo_updates = 0;
  std::uint64_t trades = 0;
  std::uint64_t zero_length_frames = 0;
  std::uint64_t index = 0;
  itch::FrameStatus end_status = itch::FrameStatus::End;

  const auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    std::span<const std::uint8_t> payload;
    const itch::FrameStatus fs = reader.next(payload);
    if (fs == itch::FrameStatus::ZeroLength) {
      ++zero_length_frames;
      continue;
    }
    if (fs != itch::FrameStatus::Ok) {
      end_status = fs;
      break;
    }
    const itch::ProcessResult r = handler.process(payload);
    if (pacer.mode() != itch::Pacer::Mode::Unlimited) pacer.wait(index, r.timestamp);
    ++index;

    if (r.has_trade) {
      ++trades;
      if (trades_out != nullptr) {
        const itch::TradeEvent& t = r.trade;
        std::fprintf(trades_out, "%" PRIu64 ",%u,", t.timestamp, static_cast<unsigned>(t.locate));
        put_symbol(trades_out, handler.books().book(t.locate));
        std::fprintf(trades_out, ",%c,", t.kind);
        put_price(trades_out, t.price);
        std::fprintf(trades_out, ",%" PRIu64 ",%" PRIu64 ",", t.qty, t.match_number);
        if (t.aggressor != ' ') std::fputc(t.aggressor, trades_out);
        std::fprintf(trades_out, ",%c\n", t.printable ? 'Y' : 'N');
      }
    }

    if (r.book_locate >= 0) {
      const auto locate = static_cast<std::uint16_t>(r.book_locate);
      if (locate >= last_bbo.size()) last_bbo.resize(std::size_t{locate} + 1);
      const itch::Bbo now = handler.books().bbo(locate);
      if (now != last_bbo[locate]) {
        last_bbo[locate] = now;
        ++bbo_updates;
        if (bbo_out != nullptr) {
          std::fprintf(bbo_out, "%" PRIu64 ",%u,", r.timestamp, static_cast<unsigned>(locate));
          put_symbol(bbo_out, handler.books().book(locate));
          std::fputc(',', bbo_out);
          if (now.has_bid) put_price(bbo_out, now.bid_price);
          std::fprintf(bbo_out, ",%" PRIu64 ",", now.bid_qty);
          if (now.has_ask) put_price(bbo_out, now.ask_price);
          std::fprintf(bbo_out, ",%" PRIu64 "\n", now.ask_qty);
        }
      }
    }
    if (opt.limit != 0 && index >= opt.limit) break;
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (bbo_out != nullptr) std::fclose(bbo_out);
  if (trades_out != nullptr) std::fclose(trades_out);

  const auto& books = handler.books();
  const itch::FeedCounters& c = handler.counters();
  const itch::BookCounters& bc = books.counters();
  std::vector<itch::LevelView> levels;

  if (std::FILE* f = open_out(opt.snapshot_csv)) {
    std::fputs("locate,symbol,side,level,price,qty,orders\n", f);
    for (std::size_t loc = 0; loc < books.locate_limit(); ++loc) {
      const auto locate = static_cast<std::uint16_t>(loc);
      for (const itch::Side side : {itch::Side::Buy, itch::Side::Sell}) {
        books.top(locate, side, opt.depth, levels);
        for (std::size_t i = 0; i < levels.size(); ++i) {
          std::fprintf(f, "%zu,", loc);
          put_symbol(f, books.book(locate));
          std::fprintf(f, ",%c,%zu,", static_cast<char>(side), i);
          put_price(f, levels[i].price);
          std::fprintf(f, ",%" PRIu64 ",%u\n", levels[i].qty, levels[i].orders);
        }
      }
    }
    std::fclose(f);
  }

  if (std::FILE* f = open_out(opt.summary_csv)) {
    std::fputs(
        "locate,symbol,bid_px,bid_qty,ask_px,ask_qty,spread,imbalance,microprice,"
        "trades,volume,buy_volume,sell_volume,hidden_volume,cross_volume\n",
        f);
    for (std::size_t loc = 0; loc < books.locate_limit(); ++loc) {
      const auto locate = static_cast<std::uint16_t>(loc);
      const itch::Bbo b = books.bbo(locate);
      const itch::SymbolStats* st = handler.stats(locate);
      const bool traded = st != nullptr && (st->trades != 0 || st->cross_volume != 0);
      if (!b.has_bid && !b.has_ask && !traded) continue;
      std::fprintf(f, "%zu,", loc);
      put_symbol(f, books.book(locate));
      std::fputc(',', f);
      if (b.has_bid) put_price(f, b.bid_price);
      std::fprintf(f, ",%" PRIu64 ",", b.bid_qty);
      if (b.has_ask) put_price(f, b.ask_price);
      std::fprintf(f, ",%" PRIu64 ",", b.ask_qty);
      if (const auto s = itch::spread(b)) std::fprintf(f, "%.4f", static_cast<double>(*s) / 1e4);
      std::fputc(',', f);
      if (const auto im = itch::imbalance(b)) std::fprintf(f, "%.6f", *im);
      std::fputc(',', f);
      if (const auto mp = itch::microprice(b)) std::fprintf(f, "%.6f", *mp / 1e4);
      const itch::SymbolStats z{};
      const itch::SymbolStats& s = st != nullptr ? *st : z;
      std::fprintf(f, ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                   s.trades, s.volume, s.buy_volume, s.sell_volume, s.hidden_volume,
                   s.cross_volume);
    }
    std::fclose(f);
  }

  if (std::FILE* f = open_out(opt.counts_csv)) {
    std::fputs("type,count\n", f);
    for (std::size_t t = 0; t < c.decoded_by_type.size(); ++t) {
      if (c.decoded_by_type[t] != 0) {
        std::fprintf(f, "%c,%" PRIu64 "\n", static_cast<char>(t), c.decoded_by_type[t]);
      }
    }
    std::fclose(f);
  }

  // ---- terminal summary ----
  std::printf("file              %s (%zu bytes)\n", opt.file.c_str(), file.size());
  std::printf("framing           end=%s at offset %zu, zero-length frames=%" PRIu64 "\n",
              frame_status_name(end_status), reader.offset(), zero_length_frames);
  std::printf("messages          %" PRIu64 " (decoded %" PRIu64 ")\n", c.messages, c.decoded);
  std::printf("decode rejects    unknown_type=%" PRIu64 " truncated=%" PRIu64 " oversized=%" PRIu64
              " invalid_field=%" PRIu64 " empty=%" PRIu64 "\n",
              c.unknown_type, c.truncated, c.oversized, c.invalid_field, c.empty);
  std::printf("by type          ");
  std::uint64_t unsupported = 0;
  std::size_t unsupported_kinds = 0;
  for (std::size_t t = 0; t < c.seen_by_type.size(); ++t) {
    if (c.seen_by_type[t] == 0) continue;
    if (itch::message_length(static_cast<char>(t)) != 0) {
      std::printf(" %c=%" PRIu64, static_cast<char>(t), c.seen_by_type[t]);
    } else {
      unsupported += c.seen_by_type[t];
      ++unsupported_kinds;
    }
  }
  if (unsupported != 0) {
    std::printf(" unsupported=%" PRIu64 " (%zu distinct type bytes)", unsupported,
                unsupported_kinds);
  }
  std::printf("\n");
  std::printf("book ops          adds=%" PRIu64 " reduces=%" PRIu64 " removes=%" PRIu64
              " replaces=%" PRIu64 "\n",
              bc.adds, bc.reduces, bc.removes, bc.replaces);
  std::printf("book anomalies    duplicate_order=%" PRIu64 " unknown_order=%" PRIu64
              " over_reduce=%" PRIu64 " locate_mismatch=%" PRIu64 "\n",
              bc.duplicate_order, bc.unknown_order, bc.over_reduce, c.locate_mismatch);
  std::printf("trades            %" PRIu64 " reports, %" PRIu64 " broken\n", trades,
              c.broken_trades);
  std::printf("bbo updates       %" PRIu64 "\n", bbo_updates);
  std::printf("live orders       %zu\n", books.live_orders());
  std::printf("book digest       %016" PRIx64 "\n", books.digest());
  if (pacer.mode() == itch::Pacer::Mode::Unlimited) {
    std::printf(
        "elapsed           %.3f s (%.0f msgs/s incl. output; see itch_bench for "
        "measurements)\n",
        seconds, seconds > 0 ? static_cast<double>(c.messages) / seconds : 0.0);
  } else {
    std::printf("elapsed           %.3f s (paced)\n", seconds);
  }

  std::size_t shown = 0;
  for (std::size_t loc = 0; loc < books.locate_limit() && shown < opt.show; ++loc) {
    const auto locate = static_cast<std::uint16_t>(loc);
    const itch::Bbo b = books.bbo(locate);
    if (!b.has_bid && !b.has_ask) continue;
    ++shown;
    const itch::OrderBook* book = books.book(locate);
    const std::string_view name = book->has_symbol ? book->symbol.view() : std::string_view{"?"};
    std::printf("\n%.*s (locate %zu)", static_cast<int>(name.size()), name.data(), loc);
    if (const auto s = itch::spread(b)) std::printf("  spread %.4f", static_cast<double>(*s) / 1e4);
    if (const auto im = itch::imbalance(b)) std::printf("  imbalance %+.3f", *im);
    if (const auto mp = itch::microprice(b)) std::printf("  microprice %.4f", *mp / 1e4);
    if (const itch::SymbolStats* st = handler.stats(locate)) {
      std::printf("  volume %" PRIu64 " (buy %" PRIu64 " / sell %" PRIu64 ")", st->volume,
                  st->buy_volume, st->sell_volume);
    }
    std::printf("\n  %10s %8s %12s | %-12s %-8s %-10s\n", "bid qty", "orders", "bid", "ask",
                "orders", "ask qty");
    std::vector<itch::LevelView> bids;
    std::vector<itch::LevelView> asks;
    books.top(locate, itch::Side::Buy, opt.depth, bids);
    books.top(locate, itch::Side::Sell, opt.depth, asks);
    for (std::size_t i = 0; i < std::max(bids.size(), asks.size()); ++i) {
      if (i < bids.size()) {
        std::printf("  %10" PRIu64 " %8u %12.4f |", bids[i].qty, bids[i].orders,
                    static_cast<double>(bids[i].price) / 1e4);
      } else {
        std::printf("  %10s %8s %12s |", "", "", "");
      }
      if (i < asks.size()) {
        std::printf(" %-12.4f %-8u %-10" PRIu64, static_cast<double>(asks[i].price) / 1e4,
                    asks[i].orders, asks[i].qty);
      }
      std::printf("\n");
    }
  }

  // A truncated tail is reported but is not a crash: everything before it was
  // processed. Signal it through the exit status so scripts can tell.
  return end_status == itch::FrameStatus::End || opt.limit != 0 ? 0 : 3;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse(argc, argv, opt)) {
    usage();
    return 2;
  }
  try {
    return run(opt);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "itch_replay: %s\n", e.what());
    return 1;
  }
}

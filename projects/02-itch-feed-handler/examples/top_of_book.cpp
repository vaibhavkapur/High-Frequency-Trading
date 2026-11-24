// Minimal library usage: build three messages in memory, run them through the
// feed handler, and print the resulting top of book.
//
//   ./build/example_top_of_book
#include <cstdio>
#include <vector>

#include "itch/encoder.hpp"
#include "itch/feed_handler.hpp"
#include "itch/framing.hpp"

int main() {
  using namespace itch;
  const std::uint16_t locate = 1;
  const Symbol sym = Symbol::from("DEMO");

  // A length-prefixed stream, exactly as it would appear in a file.
  std::vector<std::uint8_t> stream;
  encode_framed(AddOrder{{locate, 0, 1000}, 1, Side::Buy, 300, sym, 99'9900}, stream);
  encode_framed(AddOrder{{locate, 0, 2000}, 2, Side::Sell, 100, sym, 100'0100}, stream);
  encode_framed(OrderExecuted{{locate, 0, 3000}, 1, 100, 1}, stream);

  FeedHandler<> handler;
  LengthPrefixedReader reader(stream);
  std::span<const std::uint8_t> payload;
  while (reader.next(payload) == FrameStatus::Ok) {
    const ProcessResult r = handler.process(payload);
    if (r.has_trade) {
      std::printf("trade: %llu @ %.4f, aggressor %c\n",
                  static_cast<unsigned long long>(r.trade.qty),
                  static_cast<double>(r.trade.price) / 1e4, r.trade.aggressor);
    }
  }

  const Bbo bbo = handler.books().bbo(locate);
  std::printf("bid %llu @ %.4f | ask %llu @ %.4f\n", static_cast<unsigned long long>(bbo.bid_qty),
              static_cast<double>(bbo.bid_price) / 1e4,
              static_cast<unsigned long long>(bbo.ask_qty),
              static_cast<double>(bbo.ask_price) / 1e4);
  std::printf("spread %.4f, imbalance %+.3f, microprice %.4f\n",
              static_cast<double>(*spread(bbo)) / 1e4, *imbalance(bbo), *microprice(bbo) / 1e4);
  return 0;
}

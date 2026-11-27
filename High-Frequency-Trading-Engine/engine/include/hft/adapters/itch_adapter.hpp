#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "hft/contracts/v1/events.hpp"
#include "hft/contracts/v1/interfaces.hpp"
#include "itch/decoder.hpp"
#include "itch/feed_handler.hpp"
#include "itch/framing.hpp"
#include "itch/synth.hpp"

namespace hft::adapters {

struct ItchAdapterStats {
  std::uint64_t frames_decoded = 0;
  std::uint64_t canonical_events_emitted = 0;
  std::uint64_t decode_errors = 0;
  std::uint64_t scale_alignment_rejects = 0;
  std::uint64_t unmapped_locates = 0;
};

// Thin adapter around Project 02 (itch::FeedHandler / itch::decode / itch::LengthPrefixedReader).
// Converts Nasdaq ITCH 5.0 frames into canonical contracts::v1::Envelope(MarketDataEvent)s
// with explicit FeedOrderRef (strictly separate from outbound OUCH ClientTokens).
class ItchFeedAdapter {
 public:
  explicit ItchFeedAdapter(contracts::v1::VenueId venue,
                           std::vector<contracts::v1::InstrumentMeta> instruments,
                           std::uint16_t channel = 1);

  // Maps 1-based ITCH stock_locate (default: locate 1 -> InstrumentId 0) to canonical InstrumentId.
  void bind_locate(std::uint16_t stock_locate, contracts::v1::InstrumentId instrument);

  // Decode a single raw ITCH message payload (without 2-byte length prefix).
  contracts::v1::ErrorCode decode_payload(std::span<const std::uint8_t> payload,
                                          contracts::v1::SimTimeNs receive_time_ns,
                                          std::vector<contracts::v1::Envelope>& out_events);

  // Decode an entire length-prefixed ITCH binary buffer (e.g. from itch::generate_synthetic or file).
  contracts::v1::ErrorCode decode_stream(std::span<const std::uint8_t> framed_stream,
                                         std::vector<contracts::v1::Envelope>& out_events,
                                         std::size_t max_messages = 0);

  // Generate synthetic ITCH 5.0 stream via Project 02's itch::generate_synthetic.
  static std::vector<std::uint8_t> generate_synthetic_stream(std::uint64_t messages,
                                                             std::uint32_t symbols,
                                                             std::uint64_t seed);

  // Reference Project 02 book for differential BBO/digest verification.
  [[nodiscard]] const itch::FeedHandler<>& reference_handler() const noexcept {
    return ref_handler_;
  }
  [[nodiscard]] contracts::v1::TopOfBook reference_tob(std::uint16_t stock_locate) const;
  [[nodiscard]] const ItchAdapterStats& stats() const noexcept { return stats_; }

 private:
  std::optional<contracts::v1::InstrumentId> resolve_locate(std::uint16_t locate) const noexcept;

  contracts::v1::VenueId venue_;
  std::uint16_t channel_;
  std::vector<contracts::v1::InstrumentMeta> instruments_;
  std::unordered_map<std::uint16_t, contracts::v1::InstrumentId> locate_to_inst_;
  // Track each live feed order's (instrument, side) because ITCH Cancel/Delete/Execute/Replace
  // messages do not always repeat Side.
  struct FeedOrderMeta {
    contracts::v1::InstrumentId instrument = 0;
    contracts::v1::Side side = contracts::v1::Side::Buy;
    contracts::v1::Price price{0};
  };
  std::unordered_map<std::uint64_t, FeedOrderMeta> feed_orders_;
  itch::FeedHandler<> ref_handler_;
  contracts::v1::EventSeq next_seq_ = 1;
  ItchAdapterStats stats_{};
};

}  // namespace hft::adapters

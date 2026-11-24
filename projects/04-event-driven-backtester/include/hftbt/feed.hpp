// Feed loading: CSV (human-readable fixtures) and a compact binary format.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hftbt/market_event.hpp"

namespace hftbt {

struct FeedData {
    std::vector<std::string> symbols; // index == InstrumentId
    std::vector<MarketEvent> events;  // sorted by ts (stable)
};

// CSV: header `ts_ns,symbol,type,order_id,side,price,qty`, integer fields,
// side is B/S (may be empty for C/D/E). Lines starting with '#' are skipped.
bool parse_csv(std::string_view text, FeedData& out, std::string& err);
std::string to_csv(const FeedData& feed);

// Binary: see docs/design.md "Binary feed format". Every field is validated;
// malformed input yields an error, never undefined behaviour.
bool parse_bin(std::span<const std::byte> data, FeedData& out, std::string& err);
std::vector<std::byte> to_bin(const FeedData& feed);

// Semantic checks shared by both formats (timestamps non-decreasing, ...).
bool validate_feed(const FeedData& feed, std::string& err);

// Dispatch on extension: ".bin" is binary, anything else is CSV.
bool load_feed(const std::string& path, FeedData& out, std::string& err);
bool save_feed(const std::string& path, const FeedData& feed, std::string& err);

} // namespace hftbt

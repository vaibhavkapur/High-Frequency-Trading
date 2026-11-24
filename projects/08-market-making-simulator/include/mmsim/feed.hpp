#pragma once

#include "mmsim/types.hpp"

#include <string>
#include <vector>

namespace mmsim {

// Reads events from a binary (.mmbin) or CSV (.csv) replay file.
// Validates strictly non-decreasing timestamps and valid fields.
bool load_market_events(const std::string& path, std::vector<MarketEvent>& out,
                        double* out_tick_size_dollars, std::string& err);

bool save_market_events_bin(const std::string& path, const std::vector<MarketEvent>& events,
                            double tick_size_dollars, std::string& err);

bool save_market_events_csv(const std::string& path, const std::vector<MarketEvent>& events,
                            std::string& err);

} // namespace mmsim

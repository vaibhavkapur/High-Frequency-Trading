#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "obce/types.hpp"

namespace obce {

// I/O and serialization adapters kept strictly separate from core codecs.
Result<std::vector<uint8_t>> read_binary_file(const std::string& path);
Status write_binary_file(const std::string& path, const std::vector<uint8_t>& data);

// Deterministic JSONL fixture serialization/deserialization (no external JSON dependency)
std::string event_to_json_line(const MarketEvent& ev, uint16_t schema_version = kSchemaVersionV2);
Result<MarketEvent> parse_event_json_line(const std::string& line);

Result<std::vector<MarketEvent>> load_events_jsonl(const std::string& path);
Status save_events_jsonl(const std::string& path, const std::vector<MarketEvent>& events, uint16_t schema_version = kSchemaVersionV2);

std::string format_diagnostics_json(const DiagnosticCounters& c);

}  // namespace obce

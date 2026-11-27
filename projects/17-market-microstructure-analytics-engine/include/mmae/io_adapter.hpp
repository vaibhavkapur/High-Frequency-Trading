#pragma once

#include "mmae/feature_engine.hpp"
#include "mmae/types.hpp"

#include <iosfwd>
#include <string>
#include <vector>

namespace mmae {

struct ParsedRow {
    size_t line_number = 0;
    bool ok = false;
    ValidationErrorCode error = ValidationErrorCode::NONE;
    std::string error_detail;
    Event event;
};

// Isolated I/O and serialization adapters (kept separate from hot-path engine).
bool load_events_csv(const std::string& path, std::vector<ParsedRow>& out_rows, std::string& io_error);
bool parse_event_csv_line(const std::string& line, size_t line_number, ParsedRow& out);

bool load_config_json(const std::string& path, EngineConfig& config, std::string& error_out);

void write_features_csv_header(std::ostream& os);
void write_feature_csv_row(std::ostream& os, const FeatureRow& row);
bool write_features_csv_file(const std::string& path, const std::vector<FeatureRow>& rows, std::string& io_error);

std::string diagnostics_to_json(const Diagnostics& diag, const std::string& run_id = "");
bool write_diagnostics_json_file(const std::string& path, const Diagnostics& diag, const std::string& run_id, std::string& io_error);

}  // namespace mmae

#pragma once

#include "kbnl/benchmark_runner.hpp"
#include "kbnl/transport.hpp"

#include <string>
#include <vector>

namespace kbnl {

std::string json_escape(const std::string& input);

std::string serialize_benchmark_result_json(const BenchmarkResult& result);

std::string serialize_capability_report_json(const CapabilityReport& report, const ClockMetadata& clock);

bool write_text_file(const std::string& path, const std::string& content, std::string& out_error);

} // namespace kbnl

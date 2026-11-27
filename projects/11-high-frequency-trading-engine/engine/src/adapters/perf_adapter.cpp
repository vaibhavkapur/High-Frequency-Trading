#include "hft/adapters/perf_adapter.hpp"

namespace hft::adapters {

bool PerfLabAdapter::write_csv_artifacts(const std::string& summary_csv_path,
                                         const std::string& raw_csv_path,
                                         const std::vector<lpl::BenchmarkResult>& results) {
  const bool ok1 = lpl::write_summary_csv(summary_csv_path, results);
  const bool ok2 = lpl::write_raw_samples_csv(raw_csv_path, results);
  return ok1 && ok2;
}

}  // namespace hft::adapters

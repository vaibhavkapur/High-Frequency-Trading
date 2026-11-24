// Shared helpers for tests.
#pragma once

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "ouch/order_manager.hpp"

namespace testutil {

// A unique temp-file path that is removed when the object goes out of scope.
class TempFile {
 public:
  explicit TempFile(const std::string& stem) {
    static std::atomic<int> counter{0};
    const char* dir = std::getenv("TMPDIR");
    path_ = std::string(dir != nullptr ? dir : "/tmp") + "/ouch_" + stem + "_" +
            std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".journal";
    std::remove(path_.c_str());
  }
  ~TempFile() { std::remove(path_.c_str()); }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  [[nodiscard]] const std::string& path() const { return path_; }

 private:
  std::string path_;
};

// Field-by-field comparison of everything that defines order state.
// Returns an empty string if the two managers agree.
inline std::string diff(const ouch::OrderManager& a, const ouch::OrderManager& b) {
  if (a.orders().size() != b.orders().size()) return "order count differs";
  for (std::size_t i = 0; i < a.orders().size(); ++i) {
    const ouch::Order& x = a.orders()[i];
    const ouch::Order& y = b.orders()[i];
    const std::string tok = x.req.token.str();
    if (!(x.req == y.req)) return tok + ": request fields differ";
    if (x.state != y.state) {
      return tok + ": state " + ouch::to_string(x.state) + " vs " + ouch::to_string(y.state);
    }
    if (x.qty != y.qty || x.cum_qty != y.cum_qty || x.leaves != y.leaves) {
      return tok + ": quantities differ";
    }
    if (x.notional != y.notional) return tok + ": notional differs";
    if (x.order_ref != y.order_ref) return tok + ": order ref differs";
    if (x.pending != y.pending || x.pending_uncertain != y.pending_uncertain) {
      return tok + ": pending request differs";
    }
    if (x.replaces != y.replaces || x.replaced_by != y.replaced_by) return tok + ": links differ";
    if (x.reason != y.reason) return tok + ": reason differs";
  }
  if (a.unresolved() != b.unresolved()) return "unresolved sets differ";
  return {};
}

}  // namespace testutil

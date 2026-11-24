#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sor/types.hpp"

namespace sor {

enum class AuditEventType : std::uint8_t {
    ParentSubmitted = 0,
    RiskApproved = 1,
    RiskRejected = 2,
    RouteDecision = 3,
    ChildSent = 4,
    ChildAck = 5,
    ChildFill = 6,
    ChildCancelSent = 7,
    ChildCancelled = 8,
    ChildReplaceSent = 9,
    ChildReplaced = 10,
    ChildRejected = 11,
    ChildTimeout = 12,
    DuplicateExecIgnored = 13,
    VenueHealthChanged = 14,
    FailoverTriggered = 15,
    ParentCompleted = 16,
};

[[nodiscard]] std::string_view to_string(AuditEventType t) noexcept;

struct AuditEvent {
    std::uint64_t seq{0};
    TimestampNs timestamp_ns{0};
    AuditEventType type{AuditEventType::ParentSubmitted};
    OrderId parent_id{0};
    OrderId child_id{0};
    VenueId venue_id{kInvalidVenue};
    std::string client_order_id{};
    std::string detail{};

    [[nodiscard]] std::string to_json() const;
};

class AuditLog {
public:
    AuditLog() = default;

    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    void record(TimestampNs ts_ns, AuditEventType type, OrderId parent_id, OrderId child_id,
                VenueId venue_id, std::string_view client_order_id, std::string detail);

    [[nodiscard]] const std::vector<AuditEvent>& events() const noexcept { return events_; }
    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }
    void clear() noexcept;

    [[nodiscard]] bool write_jsonl(const std::string& path) const;

private:
    bool enabled_{true};
    std::uint64_t next_seq_{1};
    std::vector<AuditEvent> events_{};
};

}  // namespace sor

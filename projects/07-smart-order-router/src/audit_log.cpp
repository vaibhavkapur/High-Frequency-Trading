#include "sor/audit_log.hpp"

#include <fstream>
#include <sstream>

namespace sor {

namespace {

std::string json_escape(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() + 8);
    for (char c : sv) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

}  // namespace

std::string_view to_string(AuditEventType t) noexcept {
    switch (t) {
        case AuditEventType::ParentSubmitted: return "PARENT_SUBMITTED";
        case AuditEventType::RiskApproved: return "RISK_APPROVED";
        case AuditEventType::RiskRejected: return "RISK_REJECTED";
        case AuditEventType::RouteDecision: return "ROUTE_DECISION";
        case AuditEventType::ChildSent: return "CHILD_SENT";
        case AuditEventType::ChildAck: return "CHILD_ACK";
        case AuditEventType::ChildFill: return "CHILD_FILL";
        case AuditEventType::ChildCancelSent: return "CHILD_CANCEL_SENT";
        case AuditEventType::ChildCancelled: return "CHILD_CANCELLED";
        case AuditEventType::ChildReplaceSent: return "CHILD_REPLACE_SENT";
        case AuditEventType::ChildReplaced: return "CHILD_REPLACED";
        case AuditEventType::ChildRejected: return "CHILD_REJECTED";
        case AuditEventType::ChildTimeout: return "CHILD_TIMEOUT";
        case AuditEventType::DuplicateExecIgnored: return "DUPLICATE_EXEC_IGNORED";
        case AuditEventType::VenueHealthChanged: return "VENUE_HEALTH_CHANGED";
        case AuditEventType::FailoverTriggered: return "FAILOVER_TRIGGERED";
        case AuditEventType::ParentCompleted: return "PARENT_COMPLETED";
    }
    return "UNKNOWN";
}

std::string AuditEvent::to_json() const {
    std::ostringstream oss;
    oss << "{\"seq\":" << seq << ",\"ts_ns\":" << timestamp_ns << ",\"event\":\""
        << to_string(type) << "\",\"parent_id\":" << parent_id << ",\"child_id\":" << child_id
        << ",\"venue_id\":" << static_cast<int>(venue_id) << ",\"client_order_id\":\""
        << json_escape(client_order_id) << "\",\"detail\":\"" << json_escape(detail) << "\"}";
    return oss.str();
}

void AuditLog::record(TimestampNs ts_ns, AuditEventType type, OrderId parent_id, OrderId child_id,
                      VenueId venue_id, std::string_view client_order_id, std::string detail) {
    if (!enabled_) return;
    AuditEvent ev{};
    ev.seq = next_seq_++;
    ev.timestamp_ns = ts_ns;
    ev.type = type;
    ev.parent_id = parent_id;
    ev.child_id = child_id;
    ev.venue_id = venue_id;
    ev.client_order_id = std::string(client_order_id);
    ev.detail = std::move(detail);
    events_.push_back(std::move(ev));
}

void AuditLog::clear() noexcept {
    events_.clear();
    next_seq_ = 1;
}

bool AuditLog::write_jsonl(const std::string& path) const {
    std::ofstream ofs(path);
    if (!ofs) return false;
    for (const auto& ev : events_) {
        ofs << ev.to_json() << '\n';
    }
    return ofs.good();
}

}  // namespace sor

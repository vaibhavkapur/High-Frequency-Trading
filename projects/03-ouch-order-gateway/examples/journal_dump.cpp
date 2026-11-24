// Prints a gateway journal in human-readable form and the order state it implies.
//
// Usage: ouch_journal_dump <journal-path>
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

#include "ouch/event_log.hpp"

using namespace ouch;

namespace {

std::string describe(const wire::InboundMessage& m) {
  char buf[160];
  if (const auto* e = std::get_if<wire::EnterOrder>(&m)) {
    std::snprintf(buf, sizeof(buf), "EnterOrder   %s %c %u %s @ %u tif=%u", e->token.str().c_str(),
                  e->side, e->shares, e->stock.str().c_str(), e->price, e->tif);
  } else if (const auto* u = std::get_if<wire::ReplaceOrder>(&m)) {
    std::snprintf(buf, sizeof(buf), "ReplaceOrder %s -> %s shares=%u @ %u",
                  u->existing_token.str().c_str(), u->replacement_token.str().c_str(), u->shares,
                  u->price);
  } else {
    const auto& x = std::get<wire::CancelOrder>(m);
    std::snprintf(buf, sizeof(buf), "CancelOrder  %s remaining=%u", x.token.str().c_str(),
                  x.shares);
  }
  return buf;
}

std::string describe(const wire::OutboundMessage& m) {
  char buf[160];
  if (const auto* a = std::get_if<wire::Accepted>(&m)) {
    std::snprintf(buf, sizeof(buf), "Accepted     %s ref=%llu shares=%u state=%c",
                  a->token.str().c_str(), static_cast<unsigned long long>(a->order_ref), a->shares,
                  a->order_state);
  } else if (const auto* u = std::get_if<wire::Replaced>(&m)) {
    std::snprintf(buf, sizeof(buf), "Replaced     %s (was %s) ref=%llu shares=%u @ %u",
                  u->token.str().c_str(), u->previous_token.str().c_str(),
                  static_cast<unsigned long long>(u->order_ref), u->shares, u->price);
  } else if (const auto* c = std::get_if<wire::Canceled>(&m)) {
    std::snprintf(buf, sizeof(buf), "Canceled     %s -%u reason=%c", c->token.str().c_str(),
                  c->decrement_shares, c->reason);
  } else if (const auto* e = std::get_if<wire::Executed>(&m)) {
    std::snprintf(buf, sizeof(buf), "Executed     %s %u @ %u match=%llu liq=%c",
                  e->token.str().c_str(), e->shares, e->price,
                  static_cast<unsigned long long>(e->match_number), e->liquidity);
  } else if (const auto* j = std::get_if<wire::Rejected>(&m)) {
    std::snprintf(buf, sizeof(buf), "Rejected     %s reason=%c", j->token.str().c_str(), j->reason);
  } else {
    std::snprintf(buf, sizeof(buf), "SystemEvent  %c", std::get<wire::SystemEvent>(m).event_code);
  }
  return buf;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <journal-path>\n", argv[0]);
    return 2;
  }
  std::vector<Record> records;
  if (!EventLog::read_all(argv[1], records)) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  const std::uint64_t t0 = records.empty() ? 0 : records.front().ts_ns;
  for (const Record& r : records) {
    std::printf("%12.6f ms  ", static_cast<double>(r.ts_ns - t0) / 1e6);
    switch (r.kind) {
      case RecordKind::Start:
        std::printf("START\n");
        break;
      case RecordKind::SessionUp:
        std::printf("SESSION UP    session=%.*s next_seq=%llu\n",
                    static_cast<int>(r.payload.size()),
                    reinterpret_cast<const char*>(r.payload.data()),
                    static_cast<unsigned long long>(r.seq));
        break;
      case RecordKind::SessionDown:
        std::printf("SESSION DOWN  next_seq=%llu\n", static_cast<unsigned long long>(r.seq));
        break;
      case RecordKind::RequestSent:
      case RecordKind::RequestResent: {
        wire::InboundMessage m;
        const char* tag = r.kind == RecordKind::RequestSent ? "SEND  " : "RESEND";
        if (wire::decode_inbound(r.payload, m) == wire::DecodeError::None) {
          std::printf("%s        %s\n", tag, describe(m).c_str());
        } else {
          std::printf("%s        <undecodable, %zu bytes>\n", tag, r.payload.size());
        }
        break;
      }
      case RecordKind::ExchangeMsg: {
        wire::OutboundMessage m;
        if (wire::decode_outbound(r.payload, m) == wire::DecodeError::None) {
          std::printf("RECV  #%-6llu %s\n", static_cast<unsigned long long>(r.seq),
                      describe(m).c_str());
        } else {
          std::printf("RECV  #%-6llu <undecodable, %zu bytes>\n",
                      static_cast<unsigned long long>(r.seq), r.payload.size());
        }
        break;
      }
      case RecordKind::RiskReject:
        std::printf("RISK REJECT   code=%u\n", r.payload.empty() ? 0u : r.payload[0]);
        break;
      default:
        std::printf("<unknown record kind %u>\n", static_cast<unsigned>(r.kind));
        break;
    }
  }

  OrderManager oms;
  const ReplayResult res = replay(records, oms);
  std::printf("\n%zu records, %llu applied, %llu skipped; next sequence %llu; %zu orders\n",
              records.size(), static_cast<unsigned long long>(res.applied),
              static_cast<unsigned long long>(res.skipped),
              static_cast<unsigned long long>(res.next_seq), oms.orders().size());
  for (const Order& o : oms.orders()) {
    std::printf("  %-15s %c %-8s %-15s qty=%u cum=%u leaves=%u\n", o.req.token.str().c_str(),
                o.req.side, o.req.stock.str().c_str(), to_string(o.state), o.qty, o.cum_qty,
                o.leaves);
  }
  const std::string problem = oms.check_invariants();
  std::printf("invariants: %s\n", problem.empty() ? "ok" : problem.c_str());
  return problem.empty() ? 0 : 1;
}

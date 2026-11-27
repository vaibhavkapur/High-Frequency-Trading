#include "hft/adapters/ouch_adapter.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#include "hft/adapters/matching_adapter.hpp"

namespace hft::adapters {

using namespace contracts::v1;

OuchProtocolAdapter::OuchProtocolAdapter(std::vector<InstrumentMeta> instruments, std::string firm)
    : instruments_(std::move(instruments)), firm_(firm) {}

ouch::Token OuchProtocolAdapter::to_ouch_token(const ClientToken& tok) noexcept {
  return ouch::Token(std::string_view(tok.bytes.data(), tok.bytes.size()));
}

ClientToken OuchProtocolAdapter::from_ouch_token(const ouch::Token& tok) noexcept {
  return ClientToken::from_string(tok.str());
}

ouch::Symbol OuchProtocolAdapter::to_ouch_symbol(const InstrumentMeta& inst) noexcept {
  return ouch::Symbol(inst.symbol_str());
}

ErrorCode OuchProtocolAdapter::to_ouch_inbound(InstrumentId instrument, const OrderIntent& intent,
                                               ouch::wire::InboundMessage& out_msg,
                                               std::vector<std::uint8_t>* out_wire) const {
  if (instrument >= instruments_.size()) return ErrorCode::InvalidArgument;
  const auto& inst = instruments_[instrument];

  if (intent.action == OrderAction::New) {
    const auto shares = inst.qty_to_shares(intent.qty);
    if (!shares.has_value() || *shares == 0 || *shares >= ouch::kMaxShares) {
      return ErrorCode::InvalidArgument;
    }
    std::uint32_t price4 = ouch::kMarketPrice;
    if (intent.order_type == OrderType::Limit) {
      const auto p4 = inst.price_to_price4(intent.limit_price);
      if (!p4.has_value() || *p4 > ouch::kMaxLimitPrice) return ErrorCode::InvalidArgument;
      price4 = *p4;
    }

    ouch::wire::EnterOrder eo{};
    eo.token = to_ouch_token(intent.token);
    eo.side = (intent.side == Side::Buy) ? 'B' : 'S';
    eo.shares = static_cast<std::uint32_t>(*shares);
    eo.stock = to_ouch_symbol(inst);
    eo.price = price4;
    eo.tif = (intent.tif == TimeInForce::IOC) ? ouch::kTifIoc : ouch::kTifMarketHours;
    eo.firm = firm_;
    eo.display = intent.post_only ? 'P' : 'Y';
    eo.capacity = 'A';
    eo.iso = 'N';
    eo.min_qty = 0;
    eo.cross_type = 'N';
    eo.customer_type = ' ';

    if (ouch::wire::validate(eo) != 0) return ErrorCode::InvalidArgument;
    out_msg = eo;
    if (out_wire != nullptr) {
      out_wire->resize(ouch::wire::kMaxMessageSize);
      const std::size_t n = ouch::wire::encode(eo, *out_wire);
      out_wire->resize(n);
    }
    return ErrorCode::Ok;
  }

  if (intent.action == OrderAction::Cancel) {
    const auto rem_shares = inst.qty_to_shares(intent.remaining_qty_target);
    if (!rem_shares.has_value() || *rem_shares >= ouch::kMaxShares) {
      return ErrorCode::InvalidArgument;
    }
    ouch::wire::CancelOrder co{};
    co.token = to_ouch_token(intent.orig_token.empty() ? intent.token : intent.orig_token);
    co.shares = static_cast<std::uint32_t>(*rem_shares);
    out_msg = co;
    if (out_wire != nullptr) {
      out_wire->resize(ouch::wire::kMaxMessageSize);
      const std::size_t n = ouch::wire::encode(co, *out_wire);
      out_wire->resize(n);
    }
    return ErrorCode::Ok;
  }

  if (intent.action == OrderAction::Replace) {
    const auto shares = inst.qty_to_shares(intent.qty);
    const auto p4 = inst.price_to_price4(intent.limit_price);
    if (!shares.has_value() || *shares == 0 || *shares >= ouch::kMaxShares || !p4.has_value() ||
        *p4 > ouch::kMaxLimitPrice) {
      return ErrorCode::InvalidArgument;
    }
    ouch::wire::ReplaceOrder ro{};
    ro.existing_token = to_ouch_token(intent.orig_token);
    ro.replacement_token = to_ouch_token(intent.token);
    ro.shares = static_cast<std::uint32_t>(*shares);
    ro.price = *p4;
    ro.tif = (intent.tif == TimeInForce::IOC) ? ouch::kTifIoc : ouch::kTifMarketHours;
    ro.display = intent.post_only ? 'P' : 'Y';
    ro.iso = 'N';
    ro.min_qty = 0;
    if (ouch::wire::validate(ro) != 0) return ErrorCode::InvalidArgument;
    out_msg = ro;
    if (out_wire != nullptr) {
      out_wire->resize(ouch::wire::kMaxMessageSize);
      const std::size_t n = ouch::wire::encode(ro, *out_wire);
      out_wire->resize(n);
    }
    return ErrorCode::Ok;
  }

  return ErrorCode::InvalidArgument;
}

ErrorCode OuchProtocolAdapter::from_ouch_outbound(InstrumentId instrument, OrderId client_order_id,
                                                  const ouch::wire::OutboundMessage& msg,
                                                  SimTimeNs recv_time_ns,
                                                  ExecutionReport& out_report) const {
  if (instrument >= instruments_.size()) return ErrorCode::InvalidArgument;
  const auto& inst = instruments_[instrument];
  out_report = ExecutionReport{};
  out_report.client_order_id = client_order_id;
  out_report.gateway_recv_time_ns = recv_time_ns;

  if (const auto* a = std::get_if<ouch::wire::Accepted>(&msg)) {
    out_report.type = ExecReportType::Accepted;
    out_report.token = from_ouch_token(a->token);
    out_report.exchange_order_id = a->order_ref;
    out_report.side = ouch::is_buy(a->side) ? Side::Buy : Side::Sell;
    if (a->price != ouch::kMarketPrice) {
      const auto px = inst.price4_to_price(a->price);
      if (!px.has_value()) return ErrorCode::TickAlignmentError;
      out_report.exec_price = *px;
    }
    const auto qty = inst.shares_to_qty(a->shares);
    if (!qty.has_value()) return ErrorCode::LotAlignmentError;
    out_report.requested_qty = *qty;
    out_report.leaves_qty = *qty;
    out_report.exchange_time_ns = SimTimeNs{static_cast<std::int64_t>(a->timestamp)};
    return ErrorCode::Ok;
  }
  if (const auto* e = std::get_if<ouch::wire::Executed>(&msg)) {
    out_report.type = ExecReportType::PartiallyFilled;
    out_report.token = from_ouch_token(e->token);
    out_report.exec_id = e->match_number;
    const auto px = inst.price4_to_price(e->price);
    const auto qty = inst.shares_to_qty(e->shares);
    if (!px.has_value() || !qty.has_value()) return ErrorCode::TickAlignmentError;
    out_report.exec_price = *px;
    out_report.fill_qty = *qty;
    out_report.is_maker = (e->liquidity == 'A');
    out_report.exchange_time_ns = SimTimeNs{static_cast<std::int64_t>(e->timestamp)};
    return ErrorCode::Ok;
  }
  if (const auto* c = std::get_if<ouch::wire::Canceled>(&msg)) {
    out_report.type = ExecReportType::Canceled;
    out_report.token = from_ouch_token(c->token);
    out_report.exchange_time_ns = SimTimeNs{static_cast<std::int64_t>(c->timestamp)};
    return ErrorCode::Ok;
  }
  if (const auto* u = std::get_if<ouch::wire::Replaced>(&msg)) {
    out_report.type = ExecReportType::Replaced;
    out_report.token = from_ouch_token(u->token);
    out_report.exchange_order_id = u->order_ref;
    out_report.side = ouch::is_buy(u->side) ? Side::Buy : Side::Sell;
    const auto px = inst.price4_to_price(u->price);
    const auto qty = inst.shares_to_qty(u->shares);
    if (!px.has_value() || !qty.has_value()) return ErrorCode::TickAlignmentError;
    out_report.exec_price = *px;
    out_report.requested_qty = *qty;
    out_report.exchange_time_ns = SimTimeNs{static_cast<std::int64_t>(u->timestamp)};
    return ErrorCode::Ok;
  }
  if (const auto* j = std::get_if<ouch::wire::Rejected>(&msg)) {
    out_report.type = ExecReportType::Rejected;
    out_report.token = from_ouch_token(j->token);
    out_report.reject_reason = RejectReason::ExchangeRejected;
    out_report.exchange_time_ns = SimTimeNs{static_cast<std::int64_t>(j->timestamp)};
    return ErrorCode::Ok;
  }
  return ErrorCode::DecodeError;
}

ErrorCode OuchProtocolAdapter::to_ouch_outbound(InstrumentId instrument,
                                                const ExecutionReport& report,
                                                ouch::wire::OutboundMessage& out_msg,
                                                std::vector<std::uint8_t>* out_wire) const {
  if (instrument >= instruments_.size()) return ErrorCode::InvalidArgument;
  const auto& inst = instruments_[instrument];
  const std::uint64_t ts =
      report.exchange_time_ns.value >= 0
          ? static_cast<std::uint64_t>(report.exchange_time_ns.value)
          : 0ULL;

  auto encode_out = [&](const auto& m) {
    out_msg = m;
    if (out_wire != nullptr) {
      out_wire->resize(ouch::wire::kMaxMessageSize);
      const std::size_t n = ouch::wire::encode(m, *out_wire);
      out_wire->resize(n);
    }
  };

  switch (report.type) {
    case ExecReportType::Accepted: {
      ouch::wire::Accepted a{};
      a.timestamp = ts;
      a.token = to_ouch_token(report.token);
      a.side = (report.side == Side::Buy) ? 'B' : 'S';
      a.shares = static_cast<std::uint32_t>(inst.qty_to_shares(report.requested_qty).value_or(0));
      a.stock = to_ouch_symbol(inst);
      a.price = inst.price_to_price4(report.exec_price).value_or(ouch::kMarketPrice);
      a.tif = ouch::kTifMarketHours;
      a.firm = firm_;
      a.display = 'Y';
      a.order_ref = report.exchange_order_id;
      a.capacity = 'A';
      a.iso = 'N';
      a.min_qty = 0;
      a.cross_type = 'N';
      a.order_state = 'L';
      encode_out(a);
      return ErrorCode::Ok;
    }
    case ExecReportType::PartiallyFilled:
    case ExecReportType::Filled: {
      ouch::wire::Executed e{};
      e.timestamp = ts;
      e.token = to_ouch_token(report.token);
      e.shares = static_cast<std::uint32_t>(inst.qty_to_shares(report.fill_qty).value_or(0));
      e.price = inst.price_to_price4(report.exec_price).value_or(0);
      e.liquidity = report.is_maker ? 'A' : 'R';
      e.match_number = report.exec_id;
      encode_out(e);
      return ErrorCode::Ok;
    }
    case ExecReportType::Canceled: {
      ouch::wire::Canceled c{};
      c.timestamp = ts;
      c.token = to_ouch_token(report.token);
      const std::uint64_t dec_lots =
          (report.requested_qty.lots > report.cum_filled_qty.lots + report.leaves_qty.lots)
              ? (report.requested_qty.lots - report.cum_filled_qty.lots - report.leaves_qty.lots)
              : 0;
      c.decrement_shares =
          static_cast<std::uint32_t>(inst.qty_to_shares(Quantity{dec_lots}).value_or(0));
      c.reason = 'U';
      encode_out(c);
      return ErrorCode::Ok;
    }
    case ExecReportType::Rejected:
    case ExecReportType::CancelRejected: {
      ouch::wire::Rejected j{};
      j.timestamp = ts;
      j.token = to_ouch_token(report.token);
      j.reason = 'I';
      encode_out(j);
      return ErrorCode::Ok;
    }
    default:
      return ErrorCode::InvalidArgument;
  }
}

// ---- OuchJournalBridge ----

OuchJournalBridge::OuchJournalBridge(const OuchProtocolAdapter& proto) : proto_(proto) {}

bool OuchJournalBridge::open(const std::string& path, bool flush_each) {
  return log_.open(path, flush_each);
}

void OuchJournalBridge::close() { log_.close(); }

void OuchJournalBridge::flush() { log_.flush(); }

void OuchJournalBridge::record_order_sent(InstrumentId instrument, const OrderIntent& intent,
                                          std::uint64_t ts_ns, std::uint64_t seq) {
  ouch::wire::InboundMessage msg;
  std::vector<std::uint8_t> wire;
  if (proto_.to_ouch_inbound(instrument, intent, msg, &wire) != ErrorCode::Ok) return;
  oms_.on_request_sent(msg, ts_ns);
  if (log_.is_open()) {
    log_.append(ouch::RecordKind::RequestSent, ts_ns, seq, wire);
  }
}

void OuchJournalBridge::record_order_resent(InstrumentId instrument, const OrderIntent& intent,
                                            std::uint64_t ts_ns, std::uint64_t seq) {
  ouch::wire::InboundMessage msg;
  std::vector<std::uint8_t> wire;
  if (proto_.to_ouch_inbound(instrument, intent, msg, &wire) != ErrorCode::Ok) return;
  if (intent.action == OrderAction::New) {
    oms_.on_resent(OuchProtocolAdapter::to_ouch_token(intent.token));
  } else if (intent.action == OrderAction::Cancel) {
    oms_.on_cancel_resent(OuchProtocolAdapter::to_ouch_token(
        intent.orig_token.empty() ? intent.token : intent.orig_token));
  }
  if (log_.is_open()) {
    log_.append(ouch::RecordKind::RequestResent, ts_ns, seq, wire);
  }
}

void OuchJournalBridge::record_exec_report(InstrumentId instrument, const ExecutionReport& report,
                                           std::uint64_t ts_ns, std::uint64_t seq) {
  ouch::wire::OutboundMessage msg;
  std::vector<std::uint8_t> wire;
  if (proto_.to_ouch_outbound(instrument, report, msg, &wire) != ErrorCode::Ok) return;
  oms_.on_exchange(msg, ts_ns);
  if (log_.is_open()) {
    log_.append(ouch::RecordKind::ExchangeMsg, ts_ns, seq, wire);
  }
}

void OuchJournalBridge::record_session_up(std::uint64_t ts_ns, std::uint64_t seq) {
  if (log_.is_open()) {
    log_.append(ouch::RecordKind::SessionUp, ts_ns, seq);
  }
}

void OuchJournalBridge::record_session_down(std::uint64_t ts_ns) {
  oms_.on_disconnect();
  if (log_.is_open()) {
    log_.append(ouch::RecordKind::SessionDown, ts_ns, 0);
  }
}

OuchJournalBridge::ReplayCheck OuchJournalBridge::replay_and_verify(const std::string& path,
                                                                    ouch::OrderManager& out_oms) {
  ReplayCheck res{};
  std::vector<ouch::Record> records;
  if (!ouch::EventLog::read_all(path, records)) {
    res.invariant_error = "failed to read event log";
    return res;
  }
  res.records_read = records.size();
  const auto rr = ouch::replay(records, out_oms);
  res.orders_reconstructed = out_oms.orders().size();
  res.unresolved_orders = out_oms.unresolved().size();
  res.invariant_error = out_oms.check_invariants();
  res.ok = (rr.skipped == 0 && res.invariant_error.empty());
  return res;
}

// ---- OuchLoopbackSession (network-lab loopback TCP using Project 01 PoolBook) ----

OuchLoopbackResult run_ouch_loopback_session(const std::vector<InstrumentMeta>& instruments,
                                             const std::string& journal_path,
                                             bool inject_disconnect) {
  OuchLoopbackResult res{};
  if (instruments.empty()) {
    res.error = "no instruments configured";
    return res;
  }

  ouch::sim::SimConfig scfg{};
  scfg.port = 0;
  scfg.accounts = {{"TRDR01", "PASS01"}};
  for (const auto& inst : instruments) {
    scfg.symbols.push_back(inst.symbol_str());
  }
  auto bridge = std::make_unique<OuchMatchingBridge>(instruments);
  ouch::sim::ExchangeSim exch(scfg, std::move(bridge));
  if (!exch.start()) {
    res.error = "ExchangeSim::start() failed (loopback TCP port bind unavailable)";
    return res;
  }
  res.started = true;

  ouch::GatewayConfig gcfg{};
  gcfg.host = "127.0.0.1";
  gcfg.port = exch.port();
  gcfg.username = "TRDR01";
  gcfg.password = "PASS01";
  gcfg.token_prefix = "NET";
  gcfg.journal_path = journal_path;
  gcfg.reconnect_backoff_ms = 20;
  gcfg.reconnect_backoff_max_ms = 100;

  ouch::RiskLimits rlim{};
  rlim.max_order_qty = 10'000;
  rlim.max_notional = 10'000'000ULL * ouch::kPriceScale;
  rlim.price_band_bps = 2'000;
  rlim.require_reference_price = true;

  ouch::Gateway gw(gcfg, rlim);
  const ouch::Symbol sym = OuchProtocolAdapter::to_ouch_symbol(instruments[0]);
  const std::uint32_t ref_px = instruments[0].price_to_price4(Price{10'000}).value_or(1'000'000U);
  gw.risk().set_reference_price(sym, ref_px);

  gw.set_listener([&](const ouch::OrderUpdate& upd) {
    switch (upd.event) {
      case ouch::OrderEvent::Accepted: ++res.accepted; break;
      case ouch::OrderEvent::Executed: ++res.filled; break;
      case ouch::OrderEvent::Canceled: ++res.canceled; break;
      case ouch::OrderEvent::Rejected: ++res.rejected; break;
      case ouch::OrderEvent::Resent: ++res.uncertain_recovered; break;
      default: break;
    }
  });

  if (!gw.connect()) {
    exch.stop();
    res.error = "Gateway::connect() failed";
    return res;
  }

  for (int i = 0; i < 100 && !gw.active(); ++i) {
    gw.poll(10);
  }
  if (!gw.active()) {
    exch.stop();
    res.error = "Gateway login timed out";
    return res;
  }
  res.connected = true;

  // Submit resting ask, then crossing buy to trigger matching inside Project 01 PoolBook,
  // then resting bid + cancel.
  ouch::NewOrder ask{};
  ask.side = 'S';
  ask.shares = 100;
  ask.stock = sym;
  ask.price = ref_px + 100;
  const auto r1 = gw.submit(ask);
  if (r1.ok()) ++res.orders_submitted;

  ouch::NewOrder bid{};
  bid.side = 'B';
  bid.shares = 60;
  bid.stock = sym;
  bid.price = ref_px + 100;
  const auto r2 = gw.submit(bid);
  if (r2.ok()) ++res.orders_submitted;

  for (int i = 0; i < 40 && res.filled == 0; ++i) {
    gw.poll(10);
  }

  if (inject_disconnect) {
    // Drop TCP connection mid-session, submit / reconcile, and confirm clean reconnect.
    exch.kick_all();
    for (int i = 0; i < 40 && gw.active(); ++i) {
      gw.poll(10);
    }
    for (int i = 0; i < 80 && !gw.active(); ++i) {
      gw.poll(15);
    }
    res.reconnected = gw.active();
  }

  // Cancel remaining 40 shares on resting ask.
  if (r1.ok() && gw.active()) {
    (void)gw.cancel(r1.token, 0);
    for (int i = 0; i < 40 && res.canceled == 0; ++i) {
      gw.poll(10);
    }
  }

  gw.flush_journal();
  res.invariant_check = gw.orders().check_invariants();
  gw.logout();
  exch.stop();
  return res;
}

}  // namespace hft::adapters

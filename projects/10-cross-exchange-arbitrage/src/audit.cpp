#include "xarb/audit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>

#include "xarb/json_lite.hpp"

#ifdef XARB_HAS_SQLITE3
#include <sqlite3.h>
#endif

namespace xarb {

PercentileStats compute_percentiles(std::vector<double> samples) {
  PercentileStats st;
  st.count = samples.size();
  if (samples.empty()) return st;
  std::sort(samples.begin(), samples.end());
  auto pct = [&](double p) -> double {
    const double idx = p * static_cast<double>(samples.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(idx));
    const size_t hi = static_cast<size_t>(std::ceil(idx));
    const double frac = idx - static_cast<double>(lo);
    return samples[lo] * (1.0 - frac) + samples[hi] * frac;
  };
  st.min_val = samples.front();
  st.max_val = samples.back();
  st.p50 = pct(0.50);
  st.p95 = pct(0.95);
  st.p99 = pct(0.99);
  st.p999 = pct(0.999);
  const double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
  st.mean = sum / static_cast<double>(samples.size());
  return st;
}

namespace {

bool write_summary_json(const std::string& path, const ReplaySummary& s, std::string* err) {
  std::ofstream ofs(path, std::ios::trunc);
  if (!ofs) {
    if (err) *err = "failed to write " + path;
    return false;
  }
  ofs << std::fixed << std::setprecision(6);
  char hash_buf[32];
  std::snprintf(hash_buf, sizeof(hash_buf), "%016llx", static_cast<unsigned long long>(s.run_hash));

  ofs << "{\n";
  ofs << "  \"feed_path\": \"" << json_escape(s.feed_path) << "\",\n";
  ofs << "  \"scenario\": \"" << json_escape(s.scenario_name) << "\",\n";
  ofs << "  \"run_hash\": \"" << hash_buf << "\",\n";
  ofs << "  \"total_events\": " << s.total_events << ",\n";
  ofs << "  \"simulated_span_sec\": " << s.simulated_span_sec << ",\n";
  ofs << "  \"wall_time_sec\": " << s.wall_time_sec << ",\n";
  ofs << "  \"events_per_sec\": " << s.events_per_sec << ",\n";
  ofs << "  \"book_updates_per_sec\": " << s.book_updates_per_sec << ",\n";
  ofs << "  \"scanner\": {\n";
  ofs << "    \"scans_performed\": " << s.scanner.scans_performed << ",\n";
  ofs << "    \"gross_crosses_seen\": " << s.scanner.gross_crosses_seen << ",\n";
  ofs << "    \"actionable_signals\": " << s.scanner.actionable_signals << ",\n";
  ofs << "    \"mean_gross_spread_bps\": " << s.mean_gross_spread_bps << ",\n";
  ofs << "    \"mean_net_edge_bps\": " << s.mean_net_edge_bps << ",\n";
  ofs << "    \"rejections\": {\n";
  for (size_t i = 0; i < s.scanner.rejections_by_reason.size(); ++i) {
    auto r = static_cast<SignalRejectionReason>(i);
    ofs << "      \"" << rejection_reason_name(r) << "\": " << s.scanner.rejections_by_reason[i]
        << (i + 1 < s.scanner.rejections_by_reason.size() ? ",\n" : "\n");
  }
  ofs << "    }\n";
  ofs << "  },\n";
  ofs << "  \"paper\": {\n";
  ofs << "    \"trades_attempted\": " << s.paper.trades_attempted << ",\n";
  ofs << "    \"trades_completed\": " << s.paper.trades_completed << ",\n";
  ofs << "    \"both_filled_profitable\": " << s.paper.both_filled_profitable << ",\n";
  ofs << "    \"both_filled_loss_slippage\": " << s.paper.both_filled_loss_slippage << ",\n";
  ofs << "    \"one_legged_unhedged\": " << s.paper.one_legged_unhedged << ",\n";
  ofs << "    \"both_missed_expired\": " << s.paper.both_missed_expired << ",\n";
  ofs << "    \"rejected_unfunded\": " << s.paper.rejected_unfunded << ",\n";
  ofs << "    \"fill_rate\": " << s.fill_rate << ",\n";
  ofs << "    \"false_positive_rate\": " << s.false_positive_rate << ",\n";
  ofs << "    \"total_turnover_quote\": " << s.paper.total_turnover_quote << ",\n";
  ofs << "    \"total_fees_quote\": " << s.paper.total_fees_quote << ",\n";
  ofs << "    \"total_realized_matched_pnl_quote\": " << s.paper.total_realized_matched_pnl_quote << ",\n";
  ofs << "    \"total_unhedged_mtm_pnl_quote\": " << s.paper.total_unhedged_mtm_pnl_quote << ",\n";
  ofs << "    \"net_simulated_pnl_quote\": " << s.paper.net_simulated_pnl_quote << ",\n";
  ofs << "    \"max_drawdown_quote\": " << s.paper.max_drawdown_quote << ",\n";
  ofs << "    \"initial_equity_quote\": " << s.initial_equity_quote << ",\n";
  ofs << "    \"final_equity_quote\": " << s.final_equity_quote << ",\n";
  ofs << "    \"equity_delta_quote\": " << s.equity_delta_quote << ",\n";
  ofs << "    \"accounting_invariants_ok\": " << (s.accounting_invariants_ok ? "true" : "false") << "\n";
  ofs << "  },\n";
  ofs << "  \"processing_latency_ns\": {\n";
  ofs << "    \"p50\": " << s.processing_latency_ns.p50 << ",\n";
  ofs << "    \"p95\": " << s.processing_latency_ns.p95 << ",\n";
  ofs << "    \"p99\": " << s.processing_latency_ns.p99 << ",\n";
  ofs << "    \"p999\": " << s.processing_latency_ns.p999 << ",\n";
  ofs << "    \"max\": " << s.processing_latency_ns.max_val << "\n";
  ofs << "  },\n";
  ofs << "  \"venues\": [\n";
  for (size_t i = 0; i < kVenueCount; ++i) {
    const auto& v = s.venues[i];
    ofs << "    {\n";
    ofs << "      \"venue\": \"" << venue_name(v.venue) << "\",\n";
    ofs << "      \"messages_received\": " << v.messages_received << ",\n";
    ofs << "      \"deltas_applied\": " << v.deltas_applied << ",\n";
    ofs << "      \"snapshots_applied\": " << v.snapshots_applied << ",\n";
    ofs << "      \"heartbeats_received\": " << v.heartbeats_received << ",\n";
    ofs << "      \"gaps_detected\": " << v.gaps_detected << ",\n";
    ofs << "      \"checksum_mismatches\": " << v.checksum_mismatches << ",\n";
    ofs << "      \"disconnects\": " << v.disconnects << ",\n";
    ofs << "      \"resync_count\": " << v.resync_count << ",\n";
    ofs << "      \"stale_transitions\": " << v.stale_transitions << ",\n";
    ofs << "      \"total_stale_ms\": " << v.total_stale_ms << ",\n";
    ofs << "      \"total_resync_ms\": " << v.total_resync_ms << ",\n";
    ofs << "      \"reconnect_p50_ms\": " << v.reconnect_duration_ms.p50 << ",\n";
    ofs << "      \"reconnect_p99_ms\": " << v.reconnect_duration_ms.p99 << ",\n";
    ofs << "      \"staleness_p50_ms\": " << v.staleness_ms.p50 << ",\n";
    ofs << "      \"staleness_p99_ms\": " << v.staleness_ms.p99 << "\n";
    ofs << "    }" << (i + 1 < kVenueCount ? ",\n" : "\n");
  }
  ofs << "  ]\n";
  ofs << "}\n";
  return ofs.good();
}

bool write_signals_csv(const std::string& path, const InstrumentRegistry& reg,
                       const std::vector<ArbitrageSignal>& signals, std::string* err) {
  std::ofstream ofs(path, std::ios::trunc);
  if (!ofs) {
    if (err) *err = "failed to write " + path;
    return false;
  }
  ofs << "signal_id,detect_ns,symbol,buy_venue,sell_venue,status,buy_top,sell_top,buy_vwap,sell_vwap,"
         "depth_qty,funded_qty,buy_levels,sell_levels,gross_spread_bps,net_edge_bps,est_fees_quote,"
         "est_net_profit_quote,buy_age_ms,sell_age_ms\n";
  ofs << std::fixed << std::setprecision(6);
  for (const auto& s : signals) {
    const auto* inst = reg.get(s.instrument_id);
    const double tick = inst ? inst->tick_size : 0.01;
    const double lot = inst ? inst->lot_size : 1e-6;
    const std::string sym = inst ? inst->canonical_symbol : "UNKNOWN";
    ofs << s.signal_id << ',' << s.detect_ns << ',' << sym << ',' << venue_name(s.buy_venue) << ','
        << venue_name(s.sell_venue) << ',' << rejection_reason_name(s.status) << ','
        << (static_cast<double>(s.buy_top_ticks) * tick) << ',' << (static_cast<double>(s.sell_top_ticks) * tick) << ','
        << (s.buy_vwap_ticks * tick) << ',' << (s.sell_vwap_ticks * tick) << ','
        << (static_cast<double>(s.depth_executable_lots) * lot) << ','
        << (static_cast<double>(s.funded_executable_lots) * lot) << ',' << s.buy_levels_used << ','
        << s.sell_levels_used << ',' << s.gross_spread_bps << ',' << s.net_edge_bps << ',' << s.estimated_fees_quote
        << ',' << s.estimated_net_profit_quote << ',' << (static_cast<double>(s.buy_quote_age_ns) / 1e6) << ','
        << (static_cast<double>(s.sell_quote_age_ns) / 1e6) << '\n';
  }
  return ofs.good();
}

bool write_trades_csv(const std::string& path, const InstrumentRegistry& reg,
                      const std::vector<PaperTradeRecord>& trades, std::string* err) {
  std::ofstream ofs(path, std::ios::trunc);
  if (!ofs) {
    if (err) *err = "failed to write " + path;
    return false;
  }
  ofs << "trade_id,signal_id,symbol,buy_venue,sell_venue,detect_ns,buy_arrival_ns,sell_arrival_ns,"
         "target_qty,buy_filled_qty,sell_filled_qty,exp_buy_vwap,exp_sell_vwap,exec_buy_vwap,exec_sell_vwap,"
         "buy_fee_quote,sell_fee_quote,gross_spread_bps,exp_net_edge_bps,exp_net_profit_quote,"
         "realized_matched_pnl_quote,unhedged_base_delta,unhedged_mtm_pnl_quote,total_pnl_quote,"
         "slippage_vs_exp_quote,outcome\n";
  ofs << std::fixed << std::setprecision(6);
  for (const auto& t : trades) {
    const auto* inst = reg.get(t.instrument_id);
    const double tick = inst ? inst->tick_size : 0.01;
    const double lot = inst ? inst->lot_size : 1e-6;
    const std::string sym = inst ? inst->canonical_symbol : "UNKNOWN";
    ofs << t.trade_id << ',' << t.signal_id << ',' << sym << ',' << venue_name(t.buy_venue) << ','
        << venue_name(t.sell_venue) << ',' << t.detect_ns << ',' << t.buy_arrival_ns << ',' << t.sell_arrival_ns << ','
        << (static_cast<double>(t.target_lots) * lot) << ',' << (static_cast<double>(t.buy_filled_lots) * lot) << ','
        << (static_cast<double>(t.sell_filled_lots) * lot) << ',' << (t.expected_buy_vwap * tick) << ','
        << (t.expected_sell_vwap * tick) << ',' << (t.buy_exec_vwap * tick) << ',' << (t.sell_exec_vwap * tick) << ','
        << t.buy_fee_quote << ',' << t.sell_fee_quote << ',' << t.gross_spread_bps << ',' << t.expected_net_edge_bps
        << ',' << t.expected_net_profit_quote << ',' << t.realized_matched_pnl_quote << ',' << t.unhedged_base_delta
        << ',' << t.unhedged_mtm_pnl_quote << ',' << t.total_pnl_quote << ',' << t.slippage_vs_expected_quote << ','
        << execution_outcome_name(t.outcome) << '\n';
  }
  return ofs.good();
}

#ifdef XARB_HAS_SQLITE3
bool write_audit_sqlite(const std::string& path, const InstrumentRegistry& reg, const ReplaySummary& summary,
                        const std::vector<ArbitrageSignal>& signals, const std::vector<PaperTradeRecord>& trades,
                        std::string* err) {
  std::error_code ec;
  std::filesystem::remove(path, ec);

  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    if (err) *err = "failed to open sqlite db: " + path;
    if (db) sqlite3_close(db);
    return false;
  }

  const char* schema_sql =
      "PRAGMA synchronous=OFF; PRAGMA journal_mode=MEMORY;"
      "CREATE TABLE run_summary ("
      "  scenario TEXT, feed_path TEXT, run_hash TEXT, total_events INTEGER, simulated_span_sec REAL,"
      "  gross_crosses INTEGER, actionable_signals INTEGER, trades_attempted INTEGER, fill_rate REAL,"
      "  false_positive_rate REAL, net_pnl_quote REAL, max_drawdown_quote REAL"
      ");"
      "CREATE TABLE signals ("
      "  signal_id INTEGER PRIMARY KEY, detect_ns INTEGER, symbol TEXT, buy_venue TEXT, sell_venue TEXT,"
      "  status TEXT, gross_spread_bps REAL, net_edge_bps REAL, depth_qty REAL, funded_qty REAL,"
      "  est_net_profit_quote REAL"
      ");"
      "CREATE TABLE trades ("
      "  trade_id INTEGER PRIMARY KEY, signal_id INTEGER, symbol TEXT, buy_venue TEXT, sell_venue TEXT,"
      "  detect_ns INTEGER, target_qty REAL, buy_filled_qty REAL, sell_filled_qty REAL,"
      "  exp_net_profit_quote REAL, total_pnl_quote REAL, slippage_quote REAL, outcome TEXT"
      ");"
      "CREATE INDEX idx_signals_status ON signals(status);"
      "CREATE INDEX idx_trades_outcome ON trades(outcome);";

  char* sql_err = nullptr;
  if (sqlite3_exec(db, schema_sql, nullptr, nullptr, &sql_err) != SQLITE_OK) {
    if (err) *err = std::string("sqlite schema error: ") + (sql_err ? sql_err : "");
    sqlite3_free(sql_err);
    sqlite3_close(db);
    return false;
  }

  sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

  char hash_buf[32];
  std::snprintf(hash_buf, sizeof(hash_buf), "%016llx", static_cast<unsigned long long>(summary.run_hash));
  {
    sqlite3_stmt* stmt = nullptr;
    const char* q = "INSERT INTO run_summary VALUES (?,?,?,?,?,?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(db, q, -1, &stmt, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, summary.scenario_name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, summary.feed_path.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, hash_buf, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(summary.total_events));
      sqlite3_bind_double(stmt, 5, summary.simulated_span_sec);
      sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(summary.scanner.gross_crosses_seen));
      sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(summary.scanner.actionable_signals));
      sqlite3_bind_int64(stmt, 8, static_cast<sqlite3_int64>(summary.paper.trades_attempted));
      sqlite3_bind_double(stmt, 9, summary.fill_rate);
      sqlite3_bind_double(stmt, 10, summary.false_positive_rate);
      sqlite3_bind_double(stmt, 11, summary.paper.net_simulated_pnl_quote);
      sqlite3_bind_double(stmt, 12, summary.paper.max_drawdown_quote);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
  }

  {
    sqlite3_stmt* stmt = nullptr;
    const char* q = "INSERT INTO signals VALUES (?,?,?,?,?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(db, q, -1, &stmt, nullptr) == SQLITE_OK) {
      for (const auto& s : signals) {
        const auto* inst = reg.get(s.instrument_id);
        const double lot = inst ? inst->lot_size : 1e-6;
        const std::string sym = inst ? inst->canonical_symbol : "UNKNOWN";
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(s.signal_id));
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(s.detect_ns));
        sqlite3_bind_text(stmt, 3, sym.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, std::string(venue_name(s.buy_venue)).c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, std::string(venue_name(s.sell_venue)).c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, std::string(rejection_reason_name(s.status)).c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 7, s.gross_spread_bps);
        sqlite3_bind_double(stmt, 8, s.net_edge_bps);
        sqlite3_bind_double(stmt, 9, static_cast<double>(s.depth_executable_lots) * lot);
        sqlite3_bind_double(stmt, 10, static_cast<double>(s.funded_executable_lots) * lot);
        sqlite3_bind_double(stmt, 11, s.estimated_net_profit_quote);
        sqlite3_step(stmt);
      }
      sqlite3_finalize(stmt);
    }
  }

  {
    sqlite3_stmt* stmt = nullptr;
    const char* q = "INSERT INTO trades VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(db, q, -1, &stmt, nullptr) == SQLITE_OK) {
      for (const auto& t : trades) {
        const auto* inst = reg.get(t.instrument_id);
        const double lot = inst ? inst->lot_size : 1e-6;
        const std::string sym = inst ? inst->canonical_symbol : "UNKNOWN";
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(t.trade_id));
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(t.signal_id));
        sqlite3_bind_text(stmt, 3, sym.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, std::string(venue_name(t.buy_venue)).c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, std::string(venue_name(t.sell_venue)).c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(t.detect_ns));
        sqlite3_bind_double(stmt, 7, static_cast<double>(t.target_lots) * lot);
        sqlite3_bind_double(stmt, 8, static_cast<double>(t.buy_filled_lots) * lot);
        sqlite3_bind_double(stmt, 9, static_cast<double>(t.sell_filled_lots) * lot);
        sqlite3_bind_double(stmt, 10, t.expected_net_profit_quote);
        sqlite3_bind_double(stmt, 11, t.total_pnl_quote);
        sqlite3_bind_double(stmt, 12, t.slippage_vs_expected_quote);
        sqlite3_bind_text(stmt, 13, std::string(execution_outcome_name(t.outcome)).c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
      }
      sqlite3_finalize(stmt);
    }
  }

  sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
  sqlite3_close(db);
  return true;
}
#endif

}  // namespace

bool write_audit_outputs(const std::string& out_dir, const InstrumentRegistry& registry,
                         const ReplaySummary& summary, const std::vector<ArbitrageSignal>& signals,
                         const std::vector<PaperTradeRecord>& trades, bool write_sqlite, std::string* err) {
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec) {
    if (err) *err = "failed to create output directory: " + out_dir;
    return false;
  }
  if (!write_summary_json(out_dir + "/summary.json", summary, err)) return false;
  if (!write_signals_csv(out_dir + "/signals.csv", registry, signals, err)) return false;
  if (!write_trades_csv(out_dir + "/trades.csv", registry, trades, err)) return false;
#ifdef XARB_HAS_SQLITE3
  if (write_sqlite) {
    if (!write_audit_sqlite(out_dir + "/audit.sqlite", registry, summary, signals, trades, err)) return false;
  }
#else
  (void)write_sqlite;
#endif
  return true;
}

std::string format_summary_text(const ReplaySummary& s) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2);
  char hash_buf[32];
  std::snprintf(hash_buf, sizeof(hash_buf), "%016llx", static_cast<unsigned long long>(s.run_hash));

  oss << "feed               " << s.feed_path << " (" << s.total_events << " events, span " << s.simulated_span_sec
      << " s)\n";
  oss << "scenario           " << s.scenario_name << "\n";
  oss << "throughput         " << std::setprecision(1) << s.events_per_sec << " events/s ("
      << s.book_updates_per_sec << " book updates/s, wall " << std::setprecision(4) << s.wall_time_sec << " s)\n";
  oss << "proc latency       p50 " << std::setprecision(0) << s.processing_latency_ns.p50 << " ns, p99 "
      << s.processing_latency_ns.p99 << " ns, p99.9 " << s.processing_latency_ns.p999 << " ns\n";
  oss << "signals            gross crosses " << s.scanner.gross_crosses_seen << ", actionable "
      << s.scanner.actionable_signals << " (stale "
      << s.scanner.rejections_by_reason[static_cast<size_t>(SignalRejectionReason::StaleQuote)] << ", unsynced "
      << s.scanner.rejections_by_reason[static_cast<size_t>(SignalRejectionReason::VenueNotSynced)]
      << ", fees/edge "
      << s.scanner.rejections_by_reason[static_cast<size_t>(SignalRejectionReason::InsufficientEdgeAfterFees)]
      << ", depth "
      << s.scanner.rejections_by_reason[static_cast<size_t>(SignalRejectionReason::InsufficientDepth)]
      << ", unfunded "
      << s.scanner.rejections_by_reason[static_cast<size_t>(SignalRejectionReason::InsufficientFunding)]
      << ", cooldown "
      << s.scanner.rejections_by_reason[static_cast<size_t>(SignalRejectionReason::CooldownActive)] << ")\n";
  oss << std::setprecision(2);
  oss << "spreads (mean)     gross " << s.mean_gross_spread_bps << " bps vs net executable " << s.mean_net_edge_bps
      << " bps\n";
  oss << "paper trades       attempted " << s.paper.trades_attempted << ", profitable "
      << s.paper.both_filled_profitable << ", slippage-loss " << s.paper.both_filled_loss_slippage << ", one-legged "
      << s.paper.one_legged_unhedged << ", missed " << s.paper.both_missed_expired << ", unfunded "
      << s.paper.rejected_unfunded << "\n";
  oss << std::setprecision(4);
  oss << "rates              fill rate " << (s.fill_rate * 100.0) << "%, false-positive rate "
      << (s.false_positive_rate * 100.0) << "%\n";
  oss << std::setprecision(2);
  oss << "paper pnl          net " << s.paper.net_simulated_pnl_quote << " USDT (matched "
      << s.paper.total_realized_matched_pnl_quote << ", unhedged mtm " << s.paper.total_unhedged_mtm_pnl_quote
      << ", fees " << s.paper.total_fees_quote << ", max dd " << s.paper.max_drawdown_quote << ")\n";
  oss << "invariants         " << (s.accounting_invariants_ok ? "PASS" : "FAIL") << " (equity delta "
      << s.equity_delta_quote << " USDT)\n";
  for (const auto& v : s.venues) {
    oss << "venue[" << venue_name(v.venue) << "]    msgs " << v.messages_received << ", deltas " << v.deltas_applied
        << ", snaps " << v.snapshots_applied << ", gaps " << v.gaps_detected << ", crc_err " << v.checksum_mismatches
        << ", resyncs " << v.resync_count << ", stale_ms " << v.total_stale_ms << "\n";
  }
  oss << "run hash           " << hash_buf << "\n";
  return oss.str();
}

}  // namespace xarb

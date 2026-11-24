// Exchange-truth accounting: cash, inventory, fees, mark-to-market.
#pragma once

#include <vector>

#include "hftbt/order.hpp"

namespace hftbt {

struct FeeConfig {
    // Basis points of traded notional; negative = rebate.
    double maker_bps = 0.0;
    double taker_bps = 0.0;
};

struct InstrumentAccount {
    Qty position = 0;
    double cash = 0.0;     // currency; excludes nothing (fees already deducted)
    double avg_cost = 0.0; // ticks, average entry price of the open position
    double realized = 0.0; // currency, gross of fees
    double fees = 0.0;     // currency, positive = paid
    double notional = 0.0; // currency traded (turnover)
    double mark = 0.0;     // ticks, last mark price
    bool has_mark = false;
    Qty volume = 0;
    std::uint64_t maker_fills = 0;
    std::uint64_t taker_fills = 0;
};

class Portfolio {
public:
    Portfolio(std::size_t n_instruments, const FeeConfig& fees, double tick_value);

    // Returns the fee charged for this fill (currency).
    double on_fill(const ExecReport& fill);
    void set_mark(InstrumentId i, double mark_ticks) {
        acct_[i].mark = mark_ticks;
        acct_[i].has_mark = true;
    }

    const InstrumentAccount& account(InstrumentId i) const { return acct_[i]; }
    std::size_t size() const noexcept { return acct_.size(); }
    double tick_value() const noexcept { return tick_value_; }

    // Convention 1: cash + marked inventory.
    double equity() const noexcept;
    // Convention 2: realized + unrealized - fees. Must equal equity().
    double pnl_decomposed() const noexcept;
    double unrealized(InstrumentId i) const noexcept;
    double gross_exposure() const noexcept; // sum |position * mark| in currency
    double total_fees() const noexcept;
    double total_notional() const noexcept;
    Qty total_volume() const noexcept;

private:
    std::vector<InstrumentAccount> acct_;
    FeeConfig fees_;
    double tick_value_;
};

} // namespace hftbt

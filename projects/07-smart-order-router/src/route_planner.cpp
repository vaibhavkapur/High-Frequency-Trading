#include "sor/route_planner.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace sor {

namespace {

struct CandidateLevel {
    VenueId venue_id{kInvalidVenue};
    PriceTicks raw_price{kInvalidPrice};
    Quantity displayed_qty{0};
    TimestampNs quote_age_ns{0};
    double est_latency_ns{0.0};
    std::uint32_t priority{0};
    CostBreakdown cost{};
    double sort_key{0.0};
};

struct VenueAccum {
    bool touched{false};
    Quantity total_qty{0};
    PriceTicks worst_price{kInvalidPrice};
    TimestampNs quote_age_ns{0};
    double est_latency_ns{0.0};
    double weighted_raw_bps{0.0};
    double weighted_fee_bps{0.0};
    double weighted_lat_bps{0.0};
    double weighted_miss_bps{0.0};
    double weighted_fill_prob{0.0};
    double weighted_eff_price{0.0};
    double weighted_total_bps{0.0};
    double best_sort_key{std::numeric_limits<double>::infinity()};
};

CostBreakdown compute_cost(const VenueConfig& vcfg, const RouterConfig& rcfg, Side side,
                           PriceTicks raw_price, Quantity level_qty, Quantity target_qty,
                           double ref_mid_ticks, double est_latency_ns, TimestampNs quote_age_ns,
                           double health_fill_mult) noexcept {
    CostBreakdown c{};
    const double mid = ref_mid_ticks > 0.0 ? ref_mid_ticks : static_cast<double>(raw_price);
    c.effective_price_ticks = vcfg.effective_price_ticks(side, raw_price);

    const double signed_price_diff =
        (side == Side::Buy) ? (static_cast<double>(raw_price) - mid)
                            : (mid - static_cast<double>(raw_price));
    c.raw_price_cost_bps = (signed_price_diff / mid) * 10'000.0;
    c.explicit_fee_bps = (vcfg.taker_fee_ticks(raw_price) / mid) * 10'000.0;

    const double eff_latency_ms =
        (est_latency_ns + rcfg.stale_age_weight * static_cast<double>(quote_age_ns)) / 1'000'000.0;
    c.latency_risk_bps = rcfg.adverse_selection_bps_per_ms * std::max(0.0, eff_latency_ms);

    const double take_ratio =
        level_qty > 0
            ? std::clamp(static_cast<double>(std::min(target_qty, level_qty)) /
                             static_cast<double>(level_qty),
                         0.0, 1.0)
            : 1.0;

    const double decay = std::exp(-vcfg.latency_decay_per_ms * std::max(0.0, eff_latency_ms));
    const double depletion_factor = std::max(0.1, 1.0 - vcfg.depletion_sensitivity * take_ratio);
    c.estimated_fill_prob =
        std::clamp(vcfg.base_fill_prob * health_fill_mult * decay * depletion_factor, 0.05, 0.999);

    c.miss_penalty_bps = (1.0 - c.estimated_fill_prob) * rcfg.miss_penalty_bps;
    c.total_expected_cost_bps =
        c.raw_price_cost_bps + c.explicit_fee_bps + c.latency_risk_bps + c.miss_penalty_bps;
    return c;
}

double policy_sort_key(RoutingPolicy policy, const CostBreakdown& c) noexcept {
    switch (policy) {
        case RoutingPolicy::NaivePrimary:
        case RoutingPolicy::NaiveBestPrice:
            return c.raw_price_cost_bps;
        case RoutingPolicy::FeeAware:
            return c.raw_price_cost_bps + c.explicit_fee_bps;
        case RoutingPolicy::SmartCostAware:
            return c.total_expected_cost_bps;
    }
    return c.total_expected_cost_bps;
}

}  // namespace

RoutePlan RoutePlanner::plan(const ConsolidatedBook& book, const VenueHealthMonitor* health,
                             const RouteRequest& request) const noexcept {
    RoutePlan out{};
    out.requested_qty = request.qty;
    out.unroutable_qty = request.qty;
    if (request.qty <= 0 || request.limit_price <= 0) {
        return out;
    }

    const bool check_stale = cfg_.respect_staleness;
    const bool check_health = cfg_.respect_venue_health;

    double ref_mid = request.reference_mid_ticks;
    if (ref_mid <= 0.0) {
        const auto bbo = book.consolidated_bbo(request.now_ns, health, check_stale, check_health);
        ref_mid = bbo.mid_price_ticks();
    }
    if (ref_mid <= 0.0) {
        ref_mid = static_cast<double>(request.limit_price);
    }
    out.reference_mid_ticks = ref_mid;

    // Identify primary venue (lowest venue_id that is configured) for NaivePrimary baseline.
    VenueId primary_venue = kInvalidVenue;
    for (VenueId vid = 0; vid < kMaxVenues; ++vid) {
        if (book.has_venue(vid) && book.venue_config(vid).enabled) {
            primary_venue = vid;
            break;
        }
    }

    std::array<CandidateLevel, kMaxVenues * kMaxBookDepth> candidates{};
    std::size_t cand_count = 0;

    for (VenueId vid = 0; vid < kMaxVenues; ++vid) {
        if (!book.has_venue(vid)) continue;
        const auto& vcfg = book.venue_config(vid);
        const auto& vb = book.venue_book(vid);

        VenueEvaluation eval{};
        eval.venue_id = vid;
        eval.quote_age_ns = book.quote_age_ns(vid, request.now_ns);

        if (!vcfg.enabled ||
            (cfg_.policy == RoutingPolicy::NaivePrimary && vid != primary_venue)) {
            eval.reason = IneligibilityReason::Disabled;
            out.venue_evals[out.eval_count++] = eval;
            continue;
        }

        const bool excluded = (request.excluded_venues_mask & (1U << vid)) != 0;
        if (excluded || (check_health && health != nullptr &&
                         !health->is_routable(vid, request.now_ns))) {
            eval.reason = IneligibilityReason::VenueDown;
            out.venue_evals[out.eval_count++] = eval;
            continue;
        }

        if (check_stale && book.is_venue_stale(vid, request.now_ns)) {
            eval.reason = IneligibilityReason::StaleQuote;
            out.venue_evals[out.eval_count++] = eval;
            continue;
        }

        if (request.active_children_per_venue[vid] >= vcfg.max_active_children) {
            eval.reason = IneligibilityReason::ChildOrderCap;
            out.venue_evals[out.eval_count++] = eval;
            continue;
        }

        const auto& levels = (request.side == Side::Buy) ? vb.asks : vb.bids;
        const std::size_t level_count = (request.side == Side::Buy) ? vb.ask_count : vb.bid_count;
        if (level_count == 0) {
            eval.reason = IneligibilityReason::NoLiquidity;
            out.venue_evals[out.eval_count++] = eval;
            continue;
        }

        eval.best_raw_price = levels[0].price;
        eval.displayed_qty_at_best = levels[0].quantity;

        const double est_lat = (health != nullptr)
                                   ? health->estimated_one_way_latency_ns(vid, vcfg)
                                   : static_cast<double>(vcfg.base_one_way_latency_ns);
        const double fill_adj =
            (health != nullptr) ? health->fill_prob_adjustment(vid, vcfg) : 1.0;

        eval.best_cost = compute_cost(vcfg, cfg_, request.side, levels[0].price,
                                      levels[0].quantity, request.qty, ref_mid, est_lat,
                                      eval.quote_age_ns, fill_adj);

        bool any_within_limit = false;
        for (std::size_t i = 0; i < level_count; ++i) {
            const auto& lvl = levels[i];
            if (lvl.quantity <= 0) continue;
            const double eff_px = vcfg.effective_price_ticks(request.side, lvl.price);
            const double cmp_px =
                cfg_.check_limit_on_effective_price ? eff_px : static_cast<double>(lvl.price);
            const bool ok_limit = (request.side == Side::Buy)
                                      ? (cmp_px <= static_cast<double>(request.limit_price))
                                      : (cmp_px >= static_cast<double>(request.limit_price));
            if (!ok_limit) {
                break;  // Levels are sorted; subsequent levels are strictly worse.
            }
            any_within_limit = true;

            CandidateLevel cl{};
            cl.venue_id = vid;
            cl.raw_price = lvl.price;
            cl.displayed_qty = lvl.quantity;
            cl.quote_age_ns = eval.quote_age_ns;
            cl.est_latency_ns = est_lat;
            cl.priority = vcfg.priority;
            cl.cost = compute_cost(vcfg, cfg_, request.side, lvl.price, lvl.quantity, request.qty,
                                   ref_mid, est_lat, eval.quote_age_ns, fill_adj);
            cl.sort_key = policy_sort_key(cfg_.policy, cl.cost);
            candidates[cand_count++] = cl;
        }

        eval.reason =
            any_within_limit ? IneligibilityReason::Eligible : IneligibilityReason::PriceLimit;
        out.venue_evals[out.eval_count++] = eval;
    }

    if (cand_count == 0) {
        out.fok_failed = (request.tif == TimeInForce::FOK);
        return out;
    }

    std::sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(cand_count),
              [](const CandidateLevel& a, const CandidateLevel& b) {
                  constexpr double kEps = 1e-9;
                  if (std::abs(a.sort_key - b.sort_key) > kEps) {
                      return a.sort_key < b.sort_key;
                  }
                  if (std::abs(a.cost.estimated_fill_prob - b.cost.estimated_fill_prob) > kEps) {
                      return a.cost.estimated_fill_prob > b.cost.estimated_fill_prob;
                  }
                  if (a.priority != b.priority) {
                      return a.priority < b.priority;
                  }
                  return a.venue_id < b.venue_id;
              });

    // Compute per-venue allocation cap (respecting max_parent_share and available child slots).
    std::array<Quantity, kMaxVenues> venue_cap{};
    for (VenueId vid = 0; vid < kMaxVenues; ++vid) {
        if (!book.has_venue(vid)) continue;
        const auto& vcfg = book.venue_config(vid);
        const std::uint32_t avail_slots =
            (vcfg.max_active_children > request.active_children_per_venue[vid])
                ? (vcfg.max_active_children - request.active_children_per_venue[vid])
                : 0;
        const Quantity slot_cap =
            static_cast<Quantity>(avail_slots) * std::max<Quantity>(1, vcfg.max_child_qty);
        const Quantity share_cap =
            (vcfg.max_parent_share >= 1.0)
                ? request.qty
                : static_cast<Quantity>(
                      std::floor(static_cast<double>(request.qty) * vcfg.max_parent_share));
        venue_cap[vid] = std::min(slot_cap, share_cap);
    }

    std::array<VenueAccum, kMaxVenues> accums{};
    Quantity remaining = request.qty;

    for (std::size_t i = 0; i < cand_count && remaining > 0; ++i) {
        const auto& cl = candidates[i];
        const VenueId vid = cl.venue_id;
        const auto& vcfg = book.venue_config(vid);
        const Quantity cap_left = venue_cap[vid] - accums[vid].total_qty;
        if (cap_left <= 0) continue;

        Quantity take = std::min({remaining, cl.displayed_qty, cap_left});
        if (take <= 0) continue;

        // Enforce lot alignment incrementally so we never leave unusable sub-lot dust on a venue
        // when other venues can still absorb it.
        const Quantity lot = std::max<Quantity>(1, vcfg.lot_size);
        if (lot > 1) {
            const Quantity aligned_total = ((accums[vid].total_qty + take) / lot) * lot;
            take = aligned_total - accums[vid].total_qty;
            if (take <= 0) continue;
        }

        // If this is the first allocation to this venue and even taking everything available
        // wouldn't reach min_order_qty, skip only if no more levels on this venue could help.
        // We handle final min_order_qty pruning cleanly right below.
        auto& a = accums[vid];
        if (!a.touched) {
            a.touched = true;
            a.worst_price = cl.raw_price;
            a.quote_age_ns = cl.quote_age_ns;
            a.est_latency_ns = cl.est_latency_ns;
            a.best_sort_key = cl.sort_key;
        } else {
            if (request.side == Side::Buy) {
                a.worst_price = std::max(a.worst_price, cl.raw_price);
            } else {
                a.worst_price = std::min(a.worst_price, cl.raw_price);
            }
        }

        const double w = static_cast<double>(take);
        a.total_qty += take;
        a.weighted_raw_bps += w * cl.cost.raw_price_cost_bps;
        a.weighted_fee_bps += w * cl.cost.explicit_fee_bps;
        a.weighted_lat_bps += w * cl.cost.latency_risk_bps;
        a.weighted_miss_bps += w * cl.cost.miss_penalty_bps;
        a.weighted_fill_prob += w * cl.cost.estimated_fill_prob;
        a.weighted_eff_price += w * cl.cost.effective_price_ticks;
        a.weighted_total_bps += w * cl.cost.total_expected_cost_bps;

        remaining -= take;
    }

    // Order selected venues by their best sort key so emitted slices reflect routing preference.
    std::array<VenueId, kMaxVenues> ordered_venues{};
    std::size_t ordered_count = 0;
    for (VenueId vid = 0; vid < kMaxVenues; ++vid) {
        if (!accums[vid].touched) continue;
        const auto& vcfg = book.venue_config(vid);
        if (accums[vid].total_qty < vcfg.min_order_qty) {
            // Update venue evaluation to indicate BelowMinQty.
            for (std::size_t e = 0; e < out.eval_count; ++e) {
                if (out.venue_evals[e].venue_id == vid) {
                    out.venue_evals[e].reason = IneligibilityReason::BelowMinQty;
                    break;
                }
            }
            continue;
        }
        ordered_venues[ordered_count++] = vid;
    }

    std::sort(ordered_venues.begin(),
              ordered_venues.begin() + static_cast<std::ptrdiff_t>(ordered_count),
              [&accums](VenueId a, VenueId b) {
                  return accums[a].best_sort_key < accums[b].best_sort_key;
              });

    Quantity total_planned = 0;
    double weighted_cost_sum = 0.0;

    for (std::size_t idx = 0; idx < ordered_count; ++idx) {
        const VenueId vid = ordered_venues[idx];
        const auto& vcfg = book.venue_config(vid);
        const auto& a = accums[vid];

        CostBreakdown avg_cost{};
        const double denom = static_cast<double>(a.total_qty);
        avg_cost.raw_price_cost_bps = a.weighted_raw_bps / denom;
        avg_cost.explicit_fee_bps = a.weighted_fee_bps / denom;
        avg_cost.latency_risk_bps = a.weighted_lat_bps / denom;
        avg_cost.miss_penalty_bps = a.weighted_miss_bps / denom;
        avg_cost.estimated_fill_prob = a.weighted_fill_prob / denom;
        avg_cost.effective_price_ticks = a.weighted_eff_price / denom;
        avg_cost.total_expected_cost_bps = a.weighted_total_bps / denom;

        const Quantity max_child = std::max<Quantity>(vcfg.min_order_qty, vcfg.max_child_qty);
        const Quantity lot = std::max<Quantity>(1, vcfg.lot_size);
        const std::uint32_t max_slots =
            (vcfg.max_active_children > request.active_children_per_venue[vid])
                ? (vcfg.max_active_children - request.active_children_per_venue[vid])
                : 0;

        Quantity left = a.total_qty;
        std::uint32_t emitted_for_venue = 0;
        while (left >= vcfg.min_order_qty && emitted_for_venue < max_slots &&
               out.slice_count < kMaxRouteSlices) {
            Quantity chunk = std::min(left, max_child);
            if (lot > 1) {
                chunk = (chunk / lot) * lot;
            }
            if (chunk < vcfg.min_order_qty) break;

            // Avoid stranding a sub-minimum tail if we can split more evenly.
            const Quantity tail = left - chunk;
            if (tail > 0 && tail < vcfg.min_order_qty &&
                emitted_for_venue + 1 < max_slots && out.slice_count + 1 < kMaxRouteSlices) {
                Quantity adjusted = left - vcfg.min_order_qty;
                if (lot > 1) adjusted = (adjusted / lot) * lot;
                if (adjusted >= vcfg.min_order_qty && adjusted <= max_child &&
                    (left - adjusted) >= vcfg.min_order_qty) {
                    chunk = adjusted;
                }
            }

            PlannedChild pc{};
            pc.venue_id = vid;
            pc.limit_price = a.worst_price;
            pc.quantity = chunk;
            pc.quote_age_ns = a.quote_age_ns;
            pc.estimated_latency_ns = a.est_latency_ns;
            pc.cost = avg_cost;

            out.slices[out.slice_count++] = pc;
            total_planned += chunk;
            weighted_cost_sum += static_cast<double>(chunk) * avg_cost.total_expected_cost_bps;
            left -= chunk;
            ++emitted_for_venue;
        }
    }

    if (request.tif == TimeInForce::FOK && total_planned < request.qty) {
        out.slice_count = 0;
        out.planned_qty = 0;
        out.unroutable_qty = request.qty;
        out.expected_all_in_cost_bps = 0.0;
        out.fok_failed = true;
        return out;
    }

    if (!cfg_.allow_partial_sweep && total_planned < request.qty) {
        out.slice_count = 0;
        out.planned_qty = 0;
        out.unroutable_qty = request.qty;
        out.expected_all_in_cost_bps = 0.0;
        return out;
    }

    out.planned_qty = total_planned;
    out.unroutable_qty = request.qty - total_planned;
    out.expected_all_in_cost_bps =
        total_planned > 0 ? (weighted_cost_sum / static_cast<double>(total_planned)) : 0.0;
    return out;
}

std::string RoutePlanner::format_explanation(const RoutePlan& plan, const ConsolidatedBook& book) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4);
    oss << "planned=" << plan.planned_qty << "/" << plan.requested_qty
        << " ref_mid=$" << (plan.reference_mid_ticks / static_cast<double>(kPriceScale))
        << std::setprecision(2) << " exp_cost=" << plan.expected_all_in_cost_bps << "bps";
    if (plan.fok_failed) {
        oss << " [FOK_FAILED]";
    }
    oss << " | slices=[";
    for (std::size_t i = 0; i < plan.slice_count; ++i) {
        const auto& s = plan.slices[i];
        if (i > 0) oss << ", ";
        const std::string vname =
            book.has_venue(s.venue_id) ? book.venue_config(s.venue_id).name : "V?";
        oss << vname << ":" << s.quantity << "@" << std::setprecision(4)
            << ticks_to_usd(s.limit_price) << std::setprecision(2)
            << "(raw=" << s.cost.raw_price_cost_bps << ",fee=" << s.cost.explicit_fee_bps
            << ",lat=" << s.cost.latency_risk_bps << ",miss=" << s.cost.miss_penalty_bps
            << ",pfill=" << s.cost.estimated_fill_prob
            << ",tot=" << s.cost.total_expected_cost_bps << "bps)";
    }
    oss << "] | venues=[";
    for (std::size_t i = 0; i < plan.eval_count; ++i) {
        const auto& e = plan.venue_evals[i];
        if (i > 0) oss << ", ";
        const std::string vname =
            book.has_venue(e.venue_id) ? book.venue_config(e.venue_id).name : "V?";
        oss << vname << ":" << to_string(e.reason);
        if (e.reason == IneligibilityReason::StaleQuote) {
            oss << "(age_ms=" << std::setprecision(2)
                << (static_cast<double>(e.quote_age_ns) / 1e6) << ")";
        }
    }
    oss << "]";
    return oss.str();
}

}  // namespace sor

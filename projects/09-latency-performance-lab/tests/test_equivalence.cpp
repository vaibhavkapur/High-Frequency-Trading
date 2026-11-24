#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "lpl/allocator_bench.hpp"
#include "lpl/aos_soa.hpp"
#include "lpl/branch_and_sharing.hpp"
#include "lpl/dispatch.hpp"
#include "lpl/flat_hash_map.hpp"
#include "lpl/order_book_variants.hpp"

namespace {

#define REQUIRE(cond)                                                                           \
  do {                                                                                          \
    if (!(cond)) {                                                                              \
      std::cerr << "FAIL (" << __FILE__ << ":" << __LINE__ << "): " << #cond << "\n";           \
      std::exit(1);                                                                             \
    }                                                                                           \
  } while (0)

void test_aos_soa_equivalence() {
  const auto ds = lpl::make_aos_soa_dataset(4096, 123);
  REQUIRE(lpl::scan_sequential_aos(ds.aos) == lpl::scan_sequential_soa(ds.soa));
  REQUIRE(lpl::scan_random_single_field_aos(ds.aos, ds.random_indices) ==
          lpl::scan_random_single_field_soa(ds.soa, ds.random_indices));
  REQUIRE(lpl::scan_random_multi_field_aos(ds.aos, ds.random_indices) ==
          lpl::scan_random_multi_field_soa(ds.soa, ds.random_indices));
}

void test_flat_hash_map() {
  lpl::FlatHashMap<uint64_t, uint64_t> fmap(16);
  std::unordered_map<uint64_t, uint64_t> ref;

  std::mt19937_64 rng(99);
  for (size_t i = 0; i < 5000; ++i) {
    const uint64_t k = rng() % 400;
    const uint64_t v = rng();
    if ((rng() % 3) != 0) {
      fmap.insert_or_assign(k, v);
      ref[k] = v;
    } else {
      const bool e1 = fmap.erase(k);
      const bool e2 = (ref.erase(k) > 0);
      REQUIRE(e1 == e2);
    }
    REQUIRE(fmap.size() == ref.size());
  }

  for (const auto& [k, v] : ref) {
    const uint64_t* found = fmap.find(k);
    REQUIRE(found != nullptr);
    REQUIRE(*found == v);
  }

  // Negative case: reserved empty key must throw.
  bool threw = false;
  try {
    fmap.insert_or_assign(std::numeric_limits<uint64_t>::max(), 1);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  REQUIRE(threw);
}

void test_order_book_equivalence() {
  for (size_t depth : {16UL, 64UL, 256UL}) {
    const auto events = lpl::generate_order_book_workload(4000, depth, 777 + depth);
    lpl::MapPriceBook m;
    lpl::SortedVectorPriceBook v;
    lpl::UnorderedMapPriceBook u;
    lpl::FlatHashPriceBook f(depth * 4);

    const uint64_t cm = lpl::replay_price_book(m, events);
    const uint64_t cv = lpl::replay_price_book(v, events);
    const uint64_t cu = lpl::replay_price_book(u, events);
    const uint64_t cf = lpl::replay_price_book(f, events);

    REQUIRE(cm == cv);
    REQUIRE(cm == cu);
    REQUIRE(cm == cf);
    REQUIRE(m.num_levels() == v.num_levels());
    REQUIRE(m.num_levels() == u.num_levels());
    REQUIRE(m.num_levels() == f.num_levels());
    REQUIRE(m.best_bid().price == v.best_bid().price);
    REQUIRE(m.best_bid().qty == f.best_bid().qty);
  }

  // Edge cases: empty book, zero-qty deletion, non-existent cancel.
  lpl::SortedVectorPriceBook vb;
  lpl::FlatHashPriceBook fb;
  REQUIRE(vb.best_bid().price == 0 && vb.best_bid().qty == 0);
  REQUIRE(fb.best_bid().price == 0 && fb.best_bid().qty == 0);
  vb.cancel_level(999);
  fb.cancel_level(999);
  vb.set_level(100, 50);
  fb.set_level(100, 50);
  vb.set_level(100, 0);
  fb.set_level(100, 0);
  REQUIRE(vb.num_levels() == 0 && fb.num_levels() == 0);
}

void test_allocator_equivalence_and_lifecycle() {
  const auto trace = lpl::generate_alloc_churn_trace(2048, 512, 2026);
  lpl::ObjectPool<lpl::TrackedOrder> pool(520);
  lpl::MonotonicArena arena(2100 * sizeof(lpl::TrackedOrder));

  lpl::TrackedOrder::reset_counters();
  const uint64_t c_heap = lpl::run_heap_churn(trace, 512);
  REQUIRE(lpl::TrackedOrder::live_count == 0);
  REQUIRE(lpl::TrackedOrder::total_constructed == lpl::TrackedOrder::total_destroyed);

  lpl::TrackedOrder::reset_counters();
  const uint64_t c_pool = lpl::run_pool_churn(pool, trace, 512);
  REQUIRE(lpl::TrackedOrder::live_count == 0);
  REQUIRE(lpl::TrackedOrder::total_constructed == lpl::TrackedOrder::total_destroyed);
  REQUIRE(pool.in_use() == 0);

  lpl::TrackedOrder::reset_counters();
  const uint64_t c_arena = lpl::run_arena_churn(arena, trace, 512);
  REQUIRE(lpl::TrackedOrder::live_count == 0);
  REQUIRE(lpl::TrackedOrder::total_constructed == lpl::TrackedOrder::total_destroyed);
  REQUIRE(arena.bytes_used() == 0);

  REQUIRE(c_heap == c_pool);
  REQUIRE(c_heap == c_arena);

  // Negative cases: pool exhaustion returns nullptr cleanly; arena overflow throws bad_alloc.
  {
    lpl::ObjectPool<lpl::TrackedOrder> small_pool(2);
    auto* a = small_pool.create(1ULL, 100LL, 10U);
    auto* b = small_pool.create(2ULL, 101LL, 20U);
    auto* c = small_pool.create(3ULL, 102LL, 30U);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c == nullptr);
    REQUIRE(reinterpret_cast<uintptr_t>(a) % alignof(lpl::TrackedOrder) == 0);
    small_pool.destroy(a);
    small_pool.destroy(b);
  }
  {
    lpl::MonotonicArena small_arena(sizeof(lpl::TrackedOrder));
    (void)small_arena.create<lpl::TrackedOrder>(1ULL, 100LL, 10U);
    bool threw = false;
    try {
      (void)small_arena.create<lpl::TrackedOrder>(2ULL, 101LL, 20U);
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    REQUIRE(threw);
  }
  {
    bool threw_zero_pool = false;
    try {
      lpl::ObjectPool<lpl::TrackedOrder> bad(0);
    } catch (const std::invalid_argument&) {
      threw_zero_pool = true;
    }
    REQUIRE(threw_zero_pool);

    bool threw_zero_arena = false;
    try {
      lpl::MonotonicArena bad(0);
    } catch (const std::invalid_argument&) {
      threw_zero_arena = true;
    }
    REQUIRE(threw_zero_arena);
  }
}

void test_dispatch_equivalence() {
  const size_t n = 2048;
  // Monomorphic
  {
    const auto evs = lpl::generate_dispatch_events(n, false, 11);
    lpl::VirtualAlphaA va({7, 3, 0});
    std::vector<lpl::VirtualHandler*> vptrs(n, &va);
    lpl::HandlerState st{7, 3, 0};
    std::vector<lpl::FnPtrHandler> fptrs(n, {&lpl::compute_alpha_a, &st});
    lpl::StaticAlphaA sa{{7, 3, 0}};

    const uint64_t r1 = lpl::run_virtual_dispatch(evs, vptrs);
    const uint64_t r2 = lpl::run_fnptr_dispatch(evs, fptrs);
    const uint64_t r3 = lpl::run_template_monomorphic(evs, sa);
    REQUIRE(r1 == r2);
    REQUIRE(r1 == r3);
  }

  // Heterogeneous
  {
    const auto evs = lpl::generate_dispatch_events(n, true, 22);
    lpl::VirtualAlphaA va({7, 3, 0});
    lpl::VirtualAlphaB vb({11, 5, 0});
    lpl::VirtualAlphaC vc({13, 2, 0});
    std::vector<lpl::VirtualHandler*> vptrs(n);
    lpl::HandlerState sa_st{7, 3, 0}, sb_st{11, 5, 0}, sc_st{13, 2, 0};
    std::vector<lpl::FnPtrHandler> fptrs(n);
    for (size_t i = 0; i < n; ++i) {
      if (evs[i].handler_kind == 0) {
        vptrs[i] = &va;
        fptrs[i] = {&lpl::compute_alpha_a, &sa_st};
      } else if (evs[i].handler_kind == 1) {
        vptrs[i] = &vb;
        fptrs[i] = {&lpl::compute_alpha_b, &sb_st};
      } else {
        vptrs[i] = &vc;
        fptrs[i] = {&lpl::compute_alpha_c, &sc_st};
      }
    }
    std::vector<lpl::HandlerVariant> vars = {
        lpl::StaticAlphaA{{7, 3, 0}},
        lpl::StaticAlphaB{{11, 5, 0}},
        lpl::StaticAlphaC{{13, 2, 0}},
    };
    lpl::StaticAlphaA sa{{7, 3, 0}};
    lpl::StaticAlphaB sb{{11, 5, 0}};
    lpl::StaticAlphaC sc{{13, 2, 0}};

    const uint64_t r1 = lpl::run_virtual_dispatch(evs, vptrs);
    const uint64_t r2 = lpl::run_fnptr_dispatch(evs, fptrs);
    const uint64_t r3 = lpl::run_variant_dispatch(evs, vars);
    const uint64_t r4 = lpl::run_switch_static_dispatch(evs, sa, sb, sc);
    REQUIRE(r1 == r2);
    REQUIRE(r1 == r3);
    REQUIRE(r1 == r4);
  }
}

void test_branch_equivalence() {
  const auto ds = lpl::make_branch_dataset(8192, 555);
  const uint64_t s1 = lpl::branch_filter_natural_if(ds.sorted, ds.threshold);
  const uint64_t s2 = lpl::branch_filter_natural_if(ds.unsorted, ds.threshold);
  const uint64_t s3 = lpl::branch_filter_forced_branch(ds.sorted, ds.threshold);
  const uint64_t s4 = lpl::branch_filter_forced_branch(ds.unsorted, ds.threshold);
  const uint64_t s5 = lpl::branch_filter_branchless(ds.sorted, ds.threshold);
  const uint64_t s6 = lpl::branch_filter_branchless(ds.unsorted, ds.threshold);

  REQUIRE(s1 == s2);
  REQUIRE(s1 == s3);
  REQUIRE(s1 == s4);
  REQUIRE(s1 == s5);
  REQUIRE(s1 == s6);
}

}  // namespace

int main() {
  test_aos_soa_equivalence();
  test_flat_hash_map();
  test_order_book_equivalence();
  test_allocator_equivalence_and_lifecycle();
  test_dispatch_equivalence();
  test_branch_equivalence();
  std::cout << "test_equivalence: ALL PASSED\n";
  return 0;
}

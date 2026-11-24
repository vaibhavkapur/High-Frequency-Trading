// Exhaustive interleaving check of the MPSC reservation/publication protocol.
//
// The algorithm in include/lfm/mpsc_queue.hpp is transcribed into a small
// state machine in which every shared-memory access is one atomic step. A
// depth-first search then explores EVERY interleaving of P producers and one
// consumer for small configurations and checks the invariants from
// docs/design.md:
//
//   I1  a producer never writes a slot that still holds an unread value
//   I2  the consumer never reads a slot that has not been published
//   I3  each producer's values are consumed in the order it pushed them
//       (which also rules out duplication)
//   I4  no loss, deadlock or livelock: whenever no thread can take a step,
//       every value has been pushed and consumed, and such a completed state
//       is reachable from every reachable state
//
// Scope and limits - this is a model, not a proof of the C++ code:
//   * interleavings are sequentially consistent; weak-memory reorderings are
//     not explored (the acquire/release argument for those is in the design
//     doc, and ThreadSanitizer covers the real code dynamically)
//   * the model is a hand transcription, so it can diverge from the code
//   * only small bounded configurations are explored
//
// To show the checker can actually fail, the same search is run on two broken
// set-ups, each of which must report a violation: a variant in which the
// reservation CAS is split into a non-atomic compare-then-store, and the
// unmodified algorithm at capacity 1 (which the real constructor rejects).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "test_util.hpp"

namespace {

constexpr int kMaxProducers = 3;
constexpr int kMaxSlots = 4;

struct Config {
  int producers;
  int items;  // pushes per producer
  int capacity;
  bool broken_cas;
};

// Producer program counter.
enum : std::uint8_t {
  kLoadTail,    // pos = tail
  kLoadSeq,     // seq = slot[pos].seq; branch on seq - pos
  kCas,         // CAS(tail, pos, pos + 1)
  kCasStore,    // broken variant only: unconditional tail = pos + 1
  kWriteValue,  // construct value in slot
  kPublish,     // slot.seq = pos + 1
  kDone,
};

// Consumer program counter.
enum : std::uint8_t {
  kCheckSeq,   // if slot[head].seq == head + 1 proceed, else "empty"
  kReadValue,  // move value out, destroy
  kRelease,    // slot.seq = head + capacity; ++head
};

struct State {
  std::uint8_t tail;
  std::uint8_t head;
  std::uint8_t seq[kMaxSlots];
  std::uint8_t val[kMaxSlots];  // 0 = no live value, else 1 + producer * 16 + item
  struct Producer {
    std::uint8_t pc;
    std::uint8_t pos;
    std::uint8_t item;
  } p[kMaxProducers];
  std::uint8_t consumer_pc;
  std::uint8_t next[kMaxProducers];  // next item index expected from each producer
};

std::string key(const State& s) {
  return std::string(reinterpret_cast<const char*>(&s), sizeof(State));
}

struct Result {
  std::size_t states = 0;
  std::size_t terminal_states = 0;
  std::vector<std::string> violations;
};

class Checker {
 public:
  explicit Checker(const Config& cfg)
      : cfg_(cfg), mask_(static_cast<std::uint8_t>(cfg.capacity - 1)) {}

  Result run() {
    State init;
    std::memset(&init, 0, sizeof(init));  // also zeroes padding so key() is canonical
    for (int i = 0; i < cfg_.capacity; ++i) {
      init.seq[i] = static_cast<std::uint8_t>(i);
    }
    // Explore the full state graph, remembering reverse edges for I4.
    std::vector<State> states{init};
    std::unordered_map<std::string, std::uint32_t> ids{{key(init), 0}};
    std::vector<std::vector<std::uint32_t>> preds(1);
    std::vector<std::uint32_t> complete;  // states where all work is finished
    for (std::uint32_t id = 0; id < states.size(); ++id) {
      const State s = states[id];
      bool any_enabled = false;
      for (int t = 0; t <= cfg_.producers; ++t) {
        State n = s;
        const bool enabled = t < cfg_.producers ? step_producer(n, t) : step_consumer(n);
        if (!enabled) {
          continue;
        }
        any_enabled = true;
        const auto [it, inserted] =
            ids.try_emplace(key(n), static_cast<std::uint32_t>(states.size()));
        if (inserted) {
          states.push_back(n);
          preds.emplace_back();
        }
        preds[it->second].push_back(id);
      }
      if (!any_enabled) {
        ++result_.terminal_states;
        if (is_complete(s)) {
          complete.push_back(id);
        } else {
          violation("I4: stuck state with unpushed or unconsumed values");
        }
      }
    }
    result_.states = states.size();

    // I4 (liveness half): completion must be reachable from every reachable
    // state. A producer retrying on "full" is always enabled, so a livelock
    // would never show up as a stuck state; backward reachability finds it.
    std::vector<bool> can_finish(states.size(), false);
    std::vector<std::uint32_t> work = complete;
    for (std::uint32_t id : work) {
      can_finish[id] = true;
    }
    while (!work.empty()) {
      const std::uint32_t id = work.back();
      work.pop_back();
      for (std::uint32_t pred : preds[id]) {
        if (!can_finish[pred]) {
          can_finish[pred] = true;
          work.push_back(pred);
        }
      }
    }
    for (std::size_t id = 0; id < states.size(); ++id) {
      if (!can_finish[id]) {
        violation("I4: reachable state from which completion is unreachable");
        break;
      }
    }
    return result_;
  }

 private:
  void violation(const char* what) {
    if (result_.violations.size() < 8) {
      result_.violations.emplace_back(what);
    }
  }

  // Returns false if the thread has no step to take in this state.
  bool step_producer(State& s, int t) {
    State::Producer& p = s.p[t];
    const std::uint8_t idx = p.pos & mask_;
    switch (p.pc) {
      case kLoadTail:
        p.pos = s.tail;
        p.pc = kLoadSeq;
        return true;
      case kLoadSeq: {
        const int dif = static_cast<int>(s.seq[idx]) - static_cast<int>(p.pos);
        // dif < 0: full -> try_push returns false and the caller retries.
        // dif > 0: lost a race -> reload tail. Both restart from kLoadTail.
        p.pc = dif == 0 ? kCas : kLoadTail;
        return true;
      }
      case kCas:
        if (s.tail == p.pos) {
          if (cfg_.broken_cas) {
            p.pc = kCasStore;  // compare succeeded, store happens later
          } else {
            s.tail = static_cast<std::uint8_t>(p.pos + 1);
            p.pc = kWriteValue;
          }
        } else {
          p.pos = s.tail;  // failed CAS reloads the expected value
          p.pc = kLoadSeq;
        }
        return true;
      case kCasStore:
        s.tail = static_cast<std::uint8_t>(p.pos + 1);
        p.pc = kWriteValue;
        return true;
      case kWriteValue:
        if (s.val[idx] != 0) {
          violation("I1: producer overwrote a slot holding an unread value");
        }
        s.val[idx] = static_cast<std::uint8_t>(1 + t * 16 + p.item);
        p.pc = kPublish;
        return true;
      case kPublish:
        s.seq[idx] = static_cast<std::uint8_t>(p.pos + 1);
        ++p.item;
        p.pos = 0;  // dead local; canonicalise to keep the state space small
        p.pc = p.item == cfg_.items ? kDone : kLoadTail;
        return true;
      default:
        return false;
    }
  }

  bool step_consumer(State& s) {
    const std::uint8_t idx = s.head & mask_;
    switch (s.consumer_pc) {
      case kCheckSeq:
        if (s.seq[idx] != static_cast<std::uint8_t>(s.head + 1)) {
          return false;  // try_pop returns false; retrying changes nothing
        }
        s.consumer_pc = kReadValue;
        return true;
      case kReadValue: {
        const std::uint8_t v = s.val[idx];
        if (v == 0) {
          violation("I2: consumer read an unpublished slot");
        } else {
          const int producer = (v - 1) / 16;
          const int item = (v - 1) % 16;
          if (item != s.next[producer]) {
            violation("I3: per-producer order violated (reorder, loss or duplicate)");
          }
          s.next[producer] = static_cast<std::uint8_t>(item + 1);
        }
        s.val[idx] = 0;
        s.consumer_pc = kRelease;
        return true;
      }
      default:  // kRelease
        s.seq[idx] = static_cast<std::uint8_t>(s.head + cfg_.capacity);
        ++s.head;
        s.consumer_pc = kCheckSeq;
        return true;
    }
  }

  bool is_complete(const State& s) const {
    for (int t = 0; t < cfg_.producers; ++t) {
      if (s.p[t].pc != kDone || s.next[t] != cfg_.items) {
        return false;
      }
    }
    return true;
  }

  Config cfg_;
  std::uint8_t mask_;
  Result result_;
};

void expect_correct(const Config& cfg) {
  const Result r = Checker(cfg).run();
  std::printf("  producers=%d items=%d capacity=%d: %zu states, %zu terminal, %zu violations\n",
              cfg.producers, cfg.items, cfg.capacity, r.states, r.terminal_states,
              r.violations.size());
  for (const auto& v : r.violations) {
    std::printf("    %s\n", v.c_str());
  }
  CHECK(r.violations.empty());
  CHECK(r.terminal_states >= 1);
}

void expect_broken_variant_is_caught(const Config& cfg) {
  const Result r = Checker(cfg).run();
  std::printf(
      "  [must fail] producers=%d items=%d capacity=%d: %zu states, %zu violation(s), e.g. %s\n",
      cfg.producers, cfg.items, cfg.capacity, r.states, r.violations.size(),
      r.violations.empty() ? "none" : r.violations.front().c_str());
  CHECK(!r.violations.empty());
}

}  // namespace

int main() {
  std::printf("mpsc_model_check\n");
  expect_correct({.producers = 1, .items = 4, .capacity = 2, .broken_cas = false});
  expect_correct({.producers = 2, .items = 4, .capacity = 2, .broken_cas = false});
  expect_correct({.producers = 2, .items = 3, .capacity = 2, .broken_cas = false});
  expect_correct({.producers = 2, .items = 3, .capacity = 4, .broken_cas = false});
  expect_correct({.producers = 3, .items = 2, .capacity = 2, .broken_cas = false});
  expect_correct({.producers = 3, .items = 2, .capacity = 4, .broken_cas = false});
  expect_broken_variant_is_caught({.producers = 2, .items = 2, .capacity = 2, .broken_cas = true});
  // Capacity 1 is rejected by the MpscQueue constructor; the model shows why:
  // "published" and "free for the next lap" become the same sequence value.
  expect_broken_variant_is_caught({.producers = 1, .items = 2, .capacity = 1, .broken_cas = false});
  std::printf("OK\n");
  return 0;
}

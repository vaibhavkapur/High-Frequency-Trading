#include "numa_engine/pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <numeric>
#include <regex>
#include <sstream>
#include <thread>

#include "numa_engine/clock.hpp"
#include "numa_engine/event_generator.hpp"
#include "numa_engine/json_utils.hpp"

namespace numa_engine {

namespace {

std::uint64_t fnv1a_hash(std::string_view data) noexcept {
    std::uint64_t h = 14695981039346656037ULL;
    for (char raw_c : data) {
        const auto c = static_cast<unsigned char>(raw_c);
        h ^= static_cast<std::uint64_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

std::string make_stable_id(const std::string& prefix, const std::string& material) {
    std::ostringstream oss;
    oss << prefix << "-" << std::hex << std::setw(16) << std::setfill('0') << fnv1a_hash(material);
    return oss.str();
}

void burn_consumer_work(std::uint32_t iters) noexcept {
    volatile std::uint64_t sink = 0;
    for (std::uint32_t i = 0; i < iters; ++i) {
        sink += (i * 2654435761ULL) ^ (sink >> 3);
    }
    (void)sink;
}

double median_of(std::vector<double> vals) {
    if (vals.empty()) return 0.0;
    std::sort(vals.begin(), vals.end());
    const std::size_t n = vals.size();
    if (n % 2 == 1) return vals[n / 2];
    return 0.5 * (vals[n / 2 - 1] + vals[n / 2]);
}

std::string affinity_to_json(const AffinityResult& a) {
    std::ostringstream oss;
    oss << "{"
        << "\"requested_cpu\":" << a.requested_cpu << ","
        << "\"observed_cpu\":" << a.observed_cpu << ","
        << "\"requested_pinning\":" << (a.requested_pinning ? "true" : "false") << ","
        << "\"enforced\":" << (a.enforced ? "true" : "false") << ","
        << "\"mechanism\":" << json::quote(a.mechanism) << ","
        << "\"note\":" << json::quote(a.note)
        << "}";
    return oss.str();
}

std::string allocation_to_json(const AllocationReport& r) {
    std::ostringstream oss;
    oss << "{"
        << "\"requested_policy\":" << json::quote(to_string(r.requested_policy)) << ","
        << "\"effective_policy\":" << json::quote(to_string(r.effective_policy)) << ","
        << "\"requested_node\":" << r.requested_node << ","
        << "\"effective_node\":" << r.effective_node << ","
        << "\"requested_bytes\":" << r.requested_bytes << ","
        << "\"mapped_bytes\":" << r.mapped_bytes << ","
        << "\"used_mmap\":" << (r.used_mmap ? "true" : "false") << ","
        << "\"first_touch_deferred\":" << (r.first_touch_deferred ? "true" : "false") << ","
        << "\"first_touch_completed\":" << (r.first_touch_completed ? "true" : "false") << ","
        << "\"numa_policy_enforced\":" << (r.numa_policy_enforced ? "true" : "false") << ","
        << "\"status\":" << json::quote(to_string(r.status)) << ","
        << "\"note\":" << json::quote(r.note)
        << "}";
    return oss.str();
}

std::string backpressure_to_json(const QueueBackpressureStats& b) {
    std::ostringstream oss;
    oss << "{"
        << "\"enqueued_total\":" << b.enqueued_total << ","
        << "\"dequeued_total\":" << b.dequeued_total << ","
        << "\"producer_full_spins\":" << b.producer_full_spins << ","
        << "\"consumer_empty_spins\":" << b.consumer_empty_spins << ","
        << "\"max_observed_occupancy\":" << b.max_observed_occupancy
        << "}";
    return oss.str();
}

}  // namespace

Error validate_pipeline_config(const PipelineConfig& cfg, const TopologyInfo& topology) {
    if (cfg.scenario_name.empty()) {
        return Error{ErrorCode::kInvalidConfig, "scenario_name must not be empty", "scenario_name", 0};
    }

    if (cfg.pipeline_kind == PipelineKind::kLockFreeSpsc) {
        if (!is_power_of_two(cfg.queue_capacity) ||
            cfg.queue_capacity < kMinQueueCapacity ||
            cfg.queue_capacity > kMaxQueueCapacity) {
            return Error{
                ErrorCode::kInvalidQueueCapacity,
                "SPSC queue_capacity must be a power of two in [" +
                    std::to_string(kMinQueueCapacity) + ", " +
                    std::to_string(kMaxQueueCapacity) + "]",
                "queue_capacity",
                0};
        }
    } else {
        if (cfg.queue_capacity < 1 || cfg.queue_capacity > kMaxQueueCapacity) {
            return Error{
                ErrorCode::kInvalidQueueCapacity,
                "Baseline queue_capacity must be in [1, " + std::to_string(kMaxQueueCapacity) + "]",
                "queue_capacity",
                0};
        }
    }

    if (cfg.producer_cpu >= 0 && !topology.has_cpu(cfg.producer_cpu)) {
        return Error{
            ErrorCode::kInvalidCpuId,
            "producer_cpu " + std::to_string(cfg.producer_cpu) + " is invalid for topology",
            "producer_cpu",
            0};
    }
    if (cfg.consumer_cpu >= 0 && !topology.has_cpu(cfg.consumer_cpu)) {
        return Error{
            ErrorCode::kInvalidCpuId,
            "consumer_cpu " + std::to_string(cfg.consumer_cpu) + " is invalid for topology",
            "consumer_cpu",
            0};
    }
    if (cfg.require_hard_pinning &&
        (cfg.producer_cpu >= 0 || cfg.consumer_cpu >= 0) &&
        !topology.supports_hard_cpu_pinning) {
        return Error{
            ErrorCode::kUnavailablePlatformApi,
            "Hard CPU pinning requested, but host topology does not support hard core pinning",
            "require_hard_pinning",
            0};
    }

    AllocationRequest req;
    req.bytes = cfg.queue_capacity * sizeof(MarketEvent);
    req.policy = cfg.memory_policy;
    req.preferred_node = cfg.preferred_memory_node;
    req.defer_first_touch = (cfg.memory_policy == MemoryPolicy::kFirstTouchLocal);
    req.allow_single_node_fallback = cfg.allow_single_node_fallback;

    const auto mem_eval = evaluate_memory_policy(req, topology);
    if (!mem_eval.ok()) {
        return mem_eval.error();
    }

    return Error{};
}

Result<PipelineRunResult> run_pipeline(
    const PipelineConfig& cfg,
    const std::vector<MarketEvent>& events,
    const TopologyInfo& topology) {
    const Error cfg_err = validate_pipeline_config(cfg, topology);
    if (!cfg_err.ok()) return cfg_err;

    const Error ev_err = validate_event_stream(events);
    if (!ev_err.ok()) return ev_err;

    const std::size_t total_events = events.size();
    std::size_t warmup_count = 0;
    for (const auto& ev : events) {
        if ((ev.flags & kEventFlagWarmup) != 0U) ++warmup_count;
    }
    const std::size_t measured_count = total_events - warmup_count;

    PipelineRunResult result;
    result.scenario_name = cfg.scenario_name;
    result.pipeline_kind = cfg.pipeline_kind;
    result.requested_policy = cfg.memory_policy;
    result.queue_capacity = cfg.queue_capacity;
    result.seed = cfg.seed;
    result.input_source = cfg.input_source;
    result.clock_source = std::string(MonotonicClock::clock_source_name());
    result.warmup_events = warmup_count;
    result.measured_events = measured_count;
    result.run_id = make_stable_id(
        "run",
        cfg.scenario_name + "|" + std::string(to_string(cfg.pipeline_kind)) + "|" +
            std::string(to_string(cfg.memory_policy)) + "|" + std::to_string(cfg.queue_capacity) +
            "|" + std::to_string(cfg.seed) + "|" + std::to_string(total_events));

    LatencyHistogram queue_hist(measured_count);
    LatencyHistogram e2e_hist(measured_count);
    MatchingEngine engine;
    PerfCounterCollector perf;

    std::uint64_t measure_start_ns = 0;
    std::uint64_t measure_end_ns = 0;
    std::uint64_t sent_count = 0;
    std::uint64_t recv_count = 0;
    QueueBackpressureStats bp_stats{};
    AffinityResult prod_aff{};
    AffinityResult cons_aff{};
    Error thread_error{};
    std::mutex err_mu;

    auto record_error = [&](const Error& e) {
        std::lock_guard<std::mutex> lk(err_mu);
        if (thread_error.ok()) thread_error = e;
    };

    if (cfg.pipeline_kind == PipelineKind::kBaselineMutex) {
        // Reference baseline: bounded mutex + condition_variable + std::deque
        AllocationRequest req;
        req.bytes = cfg.queue_capacity * sizeof(MarketEvent);
        req.policy = cfg.memory_policy;
        req.preferred_node = cfg.preferred_memory_node;
        req.allow_single_node_fallback = cfg.allow_single_node_fallback;
        auto eval = evaluate_memory_policy(req, topology);
        if (!eval.ok()) return eval.error();
        result.allocation = eval.value();
        result.allocation.used_mmap = false;
        result.allocation.note = "Baseline std::deque with mutex + condition_variable.";
        result.effective_policy = result.allocation.effective_policy;
        result.status = result.allocation.status;
        result.status_note = result.allocation.note;

        std::deque<MarketEvent> queue;
        std::mutex mu;
        std::condition_variable cv_not_empty;
        std::condition_variable cv_not_full;
        bool done_producing = false;

        std::atomic<bool> consumer_ready{false};
        std::atomic<bool> start_flag{false};

        std::thread consumer([&]() {
            auto aff = pin_current_thread(cfg.consumer_cpu, topology, cfg.require_hard_pinning);
            if (!aff.ok()) {
                record_error(aff.error());
                consumer_ready.store(true, std::memory_order_release);
                return;
            }
            cons_aff = aff.value();
            consumer_ready.store(true, std::memory_order_release);
            while (!start_flag.load(std::memory_order_acquire)) {
                MonotonicClock::cpu_relax();
            }

            while (true) {
                MarketEvent ev{};
                {
                    std::unique_lock<std::mutex> lk(mu);
                    if (queue.empty() && !done_producing) {
                        ++bp_stats.consumer_empty_spins;
                    }
                    cv_not_empty.wait(lk, [&]() { return !queue.empty() || done_producing; });
                    if (queue.empty() && done_producing) {
                        break;
                    }
                    ev = queue.front();
                    queue.pop_front();
                    ++bp_stats.dequeued_total;
                }
                cv_not_full.notify_one();

                const std::uint64_t recv_ts = MonotonicClock::now_ns();
                (void)engine.process(ev, recv_ts, 0);
                if (cfg.consumer_work_iters > 0) {
                    burn_consumer_work(cfg.consumer_work_iters);
                }
                const std::uint64_t done_ts = MonotonicClock::now_ns();
                ++recv_count;

                if ((ev.flags & kEventFlagWarmup) == 0U) {
                    if (measure_start_ns == 0) {
                        measure_start_ns = ev.send_ts_ns;
                    }
                    measure_end_ns = done_ts;
                    queue_hist.record(recv_ts >= ev.send_ts_ns ? (recv_ts - ev.send_ts_ns) : 0ULL);
                    e2e_hist.record(done_ts >= ev.send_ts_ns ? (done_ts - ev.send_ts_ns) : 0ULL);
                }
            }
        });

        std::thread producer([&]() {
            auto aff = pin_current_thread(cfg.producer_cpu, topology, cfg.require_hard_pinning);
            if (!aff.ok()) {
                record_error(aff.error());
            } else {
                prod_aff = aff.value();
            }
            while (!consumer_ready.load(std::memory_order_acquire)) {
                MonotonicClock::cpu_relax();
            }
            perf.start();
            start_flag.store(true, std::memory_order_release);

            if (!thread_error.ok()) {
                std::lock_guard<std::mutex> lk(mu);
                done_producing = true;
                cv_not_empty.notify_one();
                return;
            }

            for (std::size_t i = 0; i < total_events; ++i) {
                MarketEvent ev = events[i];
                ev.send_ts_ns = MonotonicClock::now_ns();
                {
                    std::unique_lock<std::mutex> lk(mu);
                    if (queue.size() >= cfg.queue_capacity) {
                        ++bp_stats.producer_full_spins;
                    }
                    cv_not_full.wait(lk, [&]() { return queue.size() < cfg.queue_capacity; });
                    queue.push_back(ev);
                    ++bp_stats.enqueued_total;
                    if (queue.size() > bp_stats.max_observed_occupancy) {
                        bp_stats.max_observed_occupancy = queue.size();
                    }
                }
                cv_not_empty.notify_one();
                ++sent_count;
            }

            {
                std::lock_guard<std::mutex> lk(mu);
                done_producing = true;
            }
            cv_not_empty.notify_one();
        });

        producer.join();
        consumer.join();
        result.perf_counters = perf.stop();
    } else {
        // NUMA-aware lock-free bounded SPSC ring buffer pipeline
        AllocationRequest req;
        req.bytes = cfg.queue_capacity * sizeof(MarketEvent);
        req.policy = cfg.memory_policy;
        req.preferred_node = cfg.preferred_memory_node;
        req.defer_first_touch = (cfg.memory_policy == MemoryPolicy::kFirstTouchLocal);
        req.allow_single_node_fallback = cfg.allow_single_node_fallback;

        auto ring_res = SpscRingBuffer<MarketEvent>::create(cfg.queue_capacity, req, topology);
        if (!ring_res.ok()) return ring_res.error();
        auto ring = std::move(ring_res.value());

        std::atomic<bool> consumer_ready{false};
        std::atomic<bool> producer_ready{false};
        std::atomic<bool> start_flag{false};

        std::thread consumer([&]() {
            auto aff = pin_current_thread(cfg.consumer_cpu, topology, cfg.require_hard_pinning);
            if (!aff.ok()) {
                record_error(aff.error());
                consumer_ready.store(true, std::memory_order_release);
                return;
            }
            cons_aff = aff.value();

            if (req.defer_first_touch && cfg.consumer_first_touch) {
                ring.first_touch_from_current_thread();
            }

            consumer_ready.store(true, std::memory_order_release);
            while (!start_flag.load(std::memory_order_acquire)) {
                MonotonicClock::cpu_relax();
            }
            if (!thread_error.ok()) return;

            MarketEvent ev{};
            while (recv_count < total_events) {
                if (ring.try_pop(ev)) {
                    const std::uint64_t recv_ts = MonotonicClock::now_ns();
                    (void)engine.process(ev, recv_ts, 0);
                    if (cfg.consumer_work_iters > 0) {
                        burn_consumer_work(cfg.consumer_work_iters);
                    }
                    const std::uint64_t done_ts = MonotonicClock::now_ns();
                    ++recv_count;

                    if ((ev.flags & kEventFlagWarmup) == 0U) {
                        if (measure_start_ns == 0) {
                            measure_start_ns = ev.send_ts_ns;
                        }
                        measure_end_ns = done_ts;
                        queue_hist.record(recv_ts >= ev.send_ts_ns ? (recv_ts - ev.send_ts_ns) : 0ULL);
                        e2e_hist.record(done_ts >= ev.send_ts_ns ? (done_ts - ev.send_ts_ns) : 0ULL);
                    }
                } else {
                    MonotonicClock::cpu_relax();
                }
            }
        });

        std::thread producer([&]() {
            auto aff = pin_current_thread(cfg.producer_cpu, topology, cfg.require_hard_pinning);
            if (!aff.ok()) {
                record_error(aff.error());
            } else {
                prod_aff = aff.value();
            }

            if (req.defer_first_touch && !cfg.consumer_first_touch) {
                ring.first_touch_from_current_thread();
            }

            producer_ready.store(true, std::memory_order_release);
            while (!consumer_ready.load(std::memory_order_acquire)) {
                MonotonicClock::cpu_relax();
            }

            perf.start();
            start_flag.store(true, std::memory_order_release);
            if (!thread_error.ok()) return;

            for (std::size_t i = 0; i < total_events; ++i) {
                MarketEvent ev = events[i];
                ev.send_ts_ns = MonotonicClock::now_ns();
                ring.push_blocking(ev);
                ++sent_count;
            }
        });

        producer.join();
        consumer.join();
        result.perf_counters = perf.stop();
        bp_stats = ring.stats();
        result.allocation = ring.allocation_report();
        result.effective_policy = result.allocation.effective_policy;
        result.status = result.allocation.status;
        result.status_note = result.allocation.note;
    }

    if (!thread_error.ok()) {
        return thread_error;
    }

    result.producer_affinity = prod_aff;
    result.consumer_affinity = cons_aff;
    result.events_sent = sent_count;
    result.events_received = recv_count;
    result.lost_events = (sent_count >= recv_count) ? (sent_count - recv_count) : (recv_count - sent_count);
    result.backpressure = bp_stats;
    result.queue_latency = queue_hist.summarize();
    result.e2e_latency = e2e_hist.summarize();
    result.engine_summary = engine.summary();

    const std::uint64_t elapsed_ns =
        (measure_end_ns > measure_start_ns) ? (measure_end_ns - measure_start_ns) : 1ULL;
    result.elapsed_ms = static_cast<double>(elapsed_ns) / 1'000'000.0;
    const std::uint64_t throughput_events = measured_count > 0 ? measured_count : total_events;
    result.throughput_eps =
        (static_cast<double>(throughput_events) * 1'000'000'000.0) / static_cast<double>(elapsed_ns);

    if (result.lost_events != 0 || result.engine_summary.total_events != total_events) {
        return Error{
            ErrorCode::kEventLossDetected,
            "Lost events detected: sent=" + std::to_string(sent_count) +
                ", received=" + std::to_string(recv_count),
            "events",
            0};
    }

    return result;
}

Result<ComparisonReport> run_comparison_suite(
    const ComparisonRunConfig& cfg,
    const TopologyInfo& topology) {
    if (cfg.repetitions == 0) {
        return Error{ErrorCode::kInvalidConfig, "repetitions must be >= 1", "repetitions", 0};
    }

    GeneratorConfig gcfg;
    gcfg.seed = cfg.seed;
    gcfg.num_events = cfg.num_events;
    gcfg.warmup_events = cfg.warmup_events;
    gcfg.num_symbols = cfg.num_symbols;

    auto events_res = generate_synthetic_events(gcfg);
    if (!events_res.ok()) return events_res.error();
    const auto& events = events_res.value();

    // Pick deterministic CPU placements from the topology
    const int node0_cpu0 = (!topology.nodes.empty() && !topology.nodes[0].cpu_ids.empty())
        ? topology.nodes[0].cpu_ids[0]
        : 0;
    const int node0_cpu1 = (!topology.nodes.empty() && topology.nodes[0].cpu_ids.size() >= 2)
        ? topology.nodes[0].cpu_ids[1]
        : node0_cpu0;
    const int remote_node = topology.is_multi_numa ? topology.nodes[1].node_id : 1;
    const int remote_cpu = (topology.is_multi_numa && !topology.nodes[1].cpu_ids.empty())
        ? topology.nodes[1].cpu_ids[0]
        : node0_cpu1;

    struct ScenarioSpec {
        std::string name;
        PipelineKind kind;
        MemoryPolicy policy;
        int prod_cpu;
        int cons_cpu;
        int mem_node;
        bool allow_fallback;
    };

    const std::vector<ScenarioSpec> specs = {
        {"baseline_mutex_unpinned", PipelineKind::kBaselineMutex, MemoryPolicy::kDefault, -1, -1, 0, true},
        {"spsc_default_unpinned", PipelineKind::kLockFreeSpsc, MemoryPolicy::kDefault, -1, -1, 0, true},
        {"spsc_local_pinned", PipelineKind::kLockFreeSpsc, MemoryPolicy::kLocal, node0_cpu0, node0_cpu1, 0, true},
        {"spsc_first_touch_local", PipelineKind::kLockFreeSpsc, MemoryPolicy::kFirstTouchLocal, node0_cpu0, node0_cpu1, 0, true},
        {"spsc_remote_node", PipelineKind::kLockFreeSpsc, MemoryPolicy::kRemote, remote_cpu, node0_cpu0, remote_node, cfg.allow_single_node_fallback},
        {"spsc_interleaved", PipelineKind::kLockFreeSpsc, MemoryPolicy::kInterleaved, node0_cpu0, node0_cpu1, 0, cfg.allow_single_node_fallback},
    };

    ComparisonReport report;
    report.build_type = NUMA_ENGINE_BUILD_TYPE;
    report.compiler = std::string(NUMA_ENGINE_COMPILER_ID) + " " + std::string(NUMA_ENGINE_COMPILER_VERSION);
    report.clock_source = std::string(MonotonicClock::clock_source_name());
    report.topology = topology;
    report.config = cfg;
    report.report_id = make_stable_id(
        "cmp",
        std::to_string(cfg.seed) + "|" + std::to_string(cfg.num_events) + "|" +
            std::to_string(cfg.queue_capacity) + "|" + std::to_string(cfg.repetitions));

    bool baseline_digest_set = false;
    bool all_equiv = true;
    bool zero_loss = true;

    for (const auto& spec : specs) {
        ScenarioAggregate agg;
        agg.scenario_name = spec.name;
        agg.pipeline_kind = spec.kind;
        agg.requested_policy = spec.policy;

        PipelineConfig pcfg;
        pcfg.scenario_name = spec.name;
        pcfg.pipeline_kind = spec.kind;
        pcfg.memory_policy = spec.policy;
        pcfg.queue_capacity = cfg.queue_capacity;
        pcfg.producer_cpu = spec.prod_cpu;
        pcfg.consumer_cpu = spec.cons_cpu;
        pcfg.preferred_memory_node = spec.mem_node;
        pcfg.consumer_first_touch = true;
        pcfg.allow_single_node_fallback = spec.allow_fallback;
        pcfg.require_hard_pinning = false;
        pcfg.consumer_work_iters = cfg.consumer_work_iters;
        pcfg.seed = cfg.seed;
        pcfg.warmup_events = cfg.warmup_events;
        pcfg.input_source = "synthetic_seed_" + std::to_string(cfg.seed);

        const Error precheck = validate_pipeline_config(pcfg, topology);
        if (!precheck.ok()) {
            if (precheck.code == ErrorCode::kUnavailableSingleNode) {
                agg.status = ScenarioStatus::kUnavailableSingleNode;
            } else if (precheck.code == ErrorCode::kUnavailablePlatformApi) {
                agg.status = ScenarioStatus::kUnavailablePlatform;
            } else {
                agg.status = ScenarioStatus::kFailedValidation;
            }
            agg.status_note = precheck.message;
            agg.repetitions = 0;
            agg.all_digests_match_baseline = true;  // Not executed; not a functional mismatch
            report.scenarios.push_back(std::move(agg));
            continue;
        }

        std::vector<double> throughputs;
        std::vector<double> p50s;
        std::vector<double> p99s;
        std::vector<double> p99_9s;
        bool scenario_matches = true;

        for (std::size_t r = 0; r < cfg.repetitions; ++r) {
            auto run_res = run_pipeline(pcfg, events, topology);
            if (!run_res.ok()) {
                return run_res.error();
            }
            const auto& run = run_res.value();
            if (!baseline_digest_set) {
                report.baseline_digest = run.engine_summary.final_state_digest;
                baseline_digest_set = true;
            }
            if (run.engine_summary.final_state_digest != report.baseline_digest) {
                scenario_matches = false;
                all_equiv = false;
            }
            if (run.lost_events != 0) {
                zero_loss = false;
            }

            agg.status = run.status;
            agg.status_note = run.status_note;
            agg.total_lost_events += run.lost_events;
            throughputs.push_back(run.throughput_eps);
            p50s.push_back(static_cast<double>(run.e2e_latency.p50_ns));
            p99s.push_back(static_cast<double>(run.e2e_latency.p99_ns));
            p99_9s.push_back(static_cast<double>(run.e2e_latency.p99_9_ns));
            agg.runs.push_back(run);
        }

        agg.repetitions = agg.runs.size();
        agg.all_digests_match_baseline = scenario_matches;
        if (!throughputs.empty()) {
            const double sum = std::accumulate(throughputs.begin(), throughputs.end(), 0.0);
            agg.mean_throughput_eps = sum / static_cast<double>(throughputs.size());
            agg.min_throughput_eps = *std::min_element(throughputs.begin(), throughputs.end());
            agg.max_throughput_eps = *std::max_element(throughputs.begin(), throughputs.end());
            agg.median_p50_ns = median_of(p50s);
            agg.median_p99_ns = median_of(p99s);
            agg.median_p99_9_ns = median_of(p99_9s);
        }

        report.scenarios.push_back(std::move(agg));
    }

    report.functional_equivalence_passed = all_equiv && baseline_digest_set;
    report.zero_lost_events = zero_loss;
    return report;
}

Result<PipelineConfig> load_pipeline_config_json(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return Error{ErrorCode::kFileNotFound, "Could not open config file: " + path, "path", 0};
    }

    std::stringstream buffer;
    buffer << ifs.rdbuf();
    const std::string content = buffer.str();
    if (content.find('{') == std::string::npos || content.find('}') == std::string::npos) {
        return Error{ErrorCode::kParseError, "Malformed JSON config (missing braces)", "json", 0};
    }

    auto find_string = [&](const std::string& key) -> std::optional<std::string> {
        const std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
        std::smatch m;
        if (std::regex_search(content, m, re)) return m[1].str();
        return std::nullopt;
    };
    auto find_int = [&](const std::string& key) -> std::optional<std::int64_t> {
        const std::regex re("\"" + key + "\"\\s*:\\s*(-?[0-9]+)");
        std::smatch m;
        if (std::regex_search(content, m, re)) return std::stoll(m[1].str());
        return std::nullopt;
    };
    auto find_bool = [&](const std::string& key) -> std::optional<bool> {
        const std::regex re("\"" + key + "\"\\s*:\\s*(true|false)");
        std::smatch m;
        if (std::regex_search(content, m, re)) return (m[1].str() == "true");
        return std::nullopt;
    };

    // Strict required fields: scenario_name, pipeline_kind, memory_policy, queue_capacity
    const auto s_name = find_string("scenario_name");
    if (!s_name || s_name->empty()) {
        return Error{ErrorCode::kInvalidConfig, "Missing required field 'scenario_name'", "scenario_name", 0};
    }
    const auto s_kind = find_string("pipeline_kind");
    if (!s_kind) {
        return Error{ErrorCode::kInvalidConfig, "Missing required field 'pipeline_kind'", "pipeline_kind", 0};
    }
    const auto kind = parse_pipeline_kind(*s_kind);
    if (!kind) {
        return Error{ErrorCode::kInvalidConfig, "Unsupported pipeline_kind: " + *s_kind, "pipeline_kind", 0};
    }
    const auto s_policy = find_string("memory_policy");
    if (!s_policy) {
        return Error{ErrorCode::kInvalidConfig, "Missing required field 'memory_policy'", "memory_policy", 0};
    }
    const auto policy = parse_memory_policy(*s_policy);
    if (!policy) {
        return Error{ErrorCode::kInvalidConfig, "Unsupported memory_policy: " + *s_policy, "memory_policy", 0};
    }
    const auto q_cap = find_int("queue_capacity");
    if (!q_cap || *q_cap <= 0) {
        return Error{ErrorCode::kInvalidQueueCapacity, "Missing or invalid required field 'queue_capacity'", "queue_capacity", 0};
    }

    PipelineConfig cfg;
    cfg.scenario_name = *s_name;
    cfg.pipeline_kind = *kind;
    cfg.memory_policy = *policy;
    cfg.queue_capacity = static_cast<std::size_t>(*q_cap);

    if (const auto v = find_int("producer_cpu")) cfg.producer_cpu = static_cast<int>(*v);
    if (const auto v = find_int("consumer_cpu")) cfg.consumer_cpu = static_cast<int>(*v);
    if (const auto v = find_int("preferred_memory_node")) cfg.preferred_memory_node = static_cast<int>(*v);
    if (const auto v = find_bool("consumer_first_touch")) cfg.consumer_first_touch = *v;
    if (const auto v = find_bool("allow_single_node_fallback")) cfg.allow_single_node_fallback = *v;
    if (const auto v = find_bool("require_hard_pinning")) cfg.require_hard_pinning = *v;
    if (const auto v = find_int("consumer_work_iters"); v && *v >= 0) {
        cfg.consumer_work_iters = static_cast<std::uint32_t>(*v);
    }
    if (const auto v = find_int("seed"); v && *v >= 0) {
        cfg.seed = static_cast<std::uint64_t>(*v);
    }
    if (const auto v = find_int("warmup_events"); v && *v >= 0) {
        cfg.warmup_events = static_cast<std::size_t>(*v);
    }
    return cfg;
}

std::string PipelineRunResult::to_json() const {
    std::ostringstream oss;
    oss << "{"
        << "\"schema_version\":" << json::quote(schema_version) << ","
        << "\"run_id\":" << json::quote(run_id) << ","
        << "\"scenario_name\":" << json::quote(scenario_name) << ","
        << "\"pipeline_kind\":" << json::quote(to_string(pipeline_kind)) << ","
        << "\"requested_policy\":" << json::quote(to_string(requested_policy)) << ","
        << "\"effective_policy\":" << json::quote(to_string(effective_policy)) << ","
        << "\"status\":" << json::quote(to_string(status)) << ","
        << "\"status_note\":" << json::quote(status_note) << ","
        << "\"queue_capacity\":" << queue_capacity << ","
        << "\"seed\":" << seed << ","
        << "\"input_source\":" << json::quote(input_source) << ","
        << "\"clock_source\":" << json::quote(clock_source) << ","
        << "\"producer_affinity\":" << affinity_to_json(producer_affinity) << ","
        << "\"consumer_affinity\":" << affinity_to_json(consumer_affinity) << ","
        << "\"allocation\":" << allocation_to_json(allocation) << ","
        << "\"events_sent\":" << events_sent << ","
        << "\"events_received\":" << events_received << ","
        << "\"warmup_events\":" << warmup_events << ","
        << "\"measured_events\":" << measured_events << ","
        << "\"lost_events\":" << lost_events << ","
        << "\"elapsed_ms\":" << json::format_double(elapsed_ms, 3) << ","
        << "\"throughput_eps\":" << json::format_double(throughput_eps, 2) << ","
        << "\"queue_latency\":" << queue_latency.to_json() << ","
        << "\"e2e_latency\":" << e2e_latency.to_json() << ","
        << "\"backpressure\":" << backpressure_to_json(backpressure) << ","
        << "\"perf_counters\":" << perf_counters.to_json() << ","
        << "\"engine_summary\":" << engine_summary.to_json()
        << "}";
    return oss.str();
}

std::string ScenarioAggregate::to_json() const {
    std::ostringstream oss;
    oss << "{"
        << "\"scenario_name\":" << json::quote(scenario_name) << ","
        << "\"pipeline_kind\":" << json::quote(to_string(pipeline_kind)) << ","
        << "\"requested_policy\":" << json::quote(to_string(requested_policy)) << ","
        << "\"status\":" << json::quote(to_string(status)) << ","
        << "\"status_note\":" << json::quote(status_note) << ","
        << "\"repetitions\":" << repetitions << ","
        << "\"mean_throughput_eps\":" << json::format_double(mean_throughput_eps, 2) << ","
        << "\"min_throughput_eps\":" << json::format_double(min_throughput_eps, 2) << ","
        << "\"max_throughput_eps\":" << json::format_double(max_throughput_eps, 2) << ","
        << "\"median_p50_ns\":" << json::format_double(median_p50_ns, 1) << ","
        << "\"median_p99_ns\":" << json::format_double(median_p99_ns, 1) << ","
        << "\"median_p99_9_ns\":" << json::format_double(median_p99_9_ns, 1) << ","
        << "\"total_lost_events\":" << total_lost_events << ","
        << "\"all_digests_match_baseline\":" << (all_digests_match_baseline ? "true" : "false") << ","
        << "\"runs\":[";
    for (std::size_t i = 0; i < runs.size(); ++i) {
        if (i > 0) oss << ",";
        oss << runs[i].to_json();
    }
    oss << "]}";
    return oss.str();
}

std::string ComparisonReport::to_json() const {
    std::ostringstream oss;
    oss << "{"
        << "\"schema_version\":" << json::quote(schema_version) << ","
        << "\"report_id\":" << json::quote(report_id) << ","
        << "\"build_type\":" << json::quote(build_type) << ","
        << "\"compiler\":" << json::quote(compiler) << ","
        << "\"clock_source\":" << json::quote(clock_source) << ","
        << "\"baseline_digest\":\"" << std::hex << baseline_digest << std::dec << "\","
        << "\"functional_equivalence_passed\":" << (functional_equivalence_passed ? "true" : "false") << ","
        << "\"zero_lost_events\":" << (zero_lost_events ? "true" : "false") << ","
        << "\"config\":{"
        << "\"num_events\":" << config.num_events << ","
        << "\"warmup_events\":" << config.warmup_events << ","
        << "\"queue_capacity\":" << config.queue_capacity << ","
        << "\"num_symbols\":" << config.num_symbols << ","
        << "\"seed\":" << config.seed << ","
        << "\"repetitions\":" << config.repetitions << ","
        << "\"allow_single_node_fallback\":" << (config.allow_single_node_fallback ? "true" : "false") << ","
        << "\"consumer_work_iters\":" << config.consumer_work_iters
        << "},"
        << "\"topology\":" << topology.to_json() << ","
        << "\"scenarios\":[";
    for (std::size_t i = 0; i < scenarios.size(); ++i) {
        if (i > 0) oss << ",";
        oss << scenarios[i].to_json();
    }
    oss << "]}";
    return oss.str();
}

}  // namespace numa_engine

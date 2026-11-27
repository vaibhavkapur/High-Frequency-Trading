#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "obce/crc32.hpp"
#include "obce/delta_codec.hpp"
#include "obce/fixture_generator.hpp"
#include "obce/reference_codec.hpp"
#include "obce/zstd_adapter.hpp"

#ifndef OBCE_BUILD_TYPE
#define OBCE_BUILD_TYPE "Unknown"
#endif

#ifndef OBCE_CXX_COMPILER
#define OBCE_CXX_COMPILER "Unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct LatencyStats {
    double min_ns = 0.0;
    double p50_ns = 0.0;
    double p95_ns = 0.0;
    double p99_ns = 0.0;
    double p999_ns = 0.0;
    double mean_ns = 0.0;
};

double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    const double idx = p * static_cast<double>(sorted.size() - 1);
    const size_t lo = static_cast<size_t>(idx);
    const size_t hi = std::min(lo + 1, sorted.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

LatencyStats compute_stats(std::vector<double> samples_ns) {
    LatencyStats st{};
    if (samples_ns.empty()) return st;
    std::sort(samples_ns.begin(), samples_ns.end());
    st.min_ns = samples_ns.front();
    st.p50_ns = percentile(samples_ns, 0.50);
    st.p95_ns = percentile(samples_ns, 0.95);
    st.p99_ns = percentile(samples_ns, 0.99);
    st.p999_ns = percentile(samples_ns, 0.999);
    const double sum = std::accumulate(samples_ns.begin(), samples_ns.end(), 0.0);
    st.mean_ns = sum / static_cast<double>(samples_ns.size());
    return st;
}

struct BenchRow {
    std::string workload;
    std::string codec;
    uint64_t seed = 0;
    size_t event_count = 0;
    uint32_t checkpoint_interval = 0;
    size_t encoded_bytes = 0;
    double bytes_per_event = 0.0;
    double compression_ratio = 1.0;
    LatencyStats encode_per_event_ns{};
    LatencyStats decode_per_event_ns{};
    double encode_mevents_per_sec = 0.0;
    double decode_mevents_per_sec = 0.0;
    LatencyStats random_seek_ns{};
    double random_seek_mean_events_scanned = 0.0;
    uint32_t input_digest = 0;
    uint32_t decoded_digest = 0;
    bool lossless_verified = false;
};

uint64_t splitmix64(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

// Measure per-batch (chunk of 256 events) encode/decode amortized ns/event over repeated runs
// to capture accurate p50/p95/p99/p99.9 latency distributions above clock resolution.
LatencyStats measure_reference_encode_latency(const std::vector<obce::MarketEvent>& events,
                                              const obce::CodecConfig& cfg,
                                              size_t chunk_size,
                                              int warmup_runs,
                                              int measured_runs) {
    std::vector<std::vector<obce::MarketEvent>> chunks;
    for (size_t i = 0; i < events.size(); i += chunk_size) {
        const size_t n = std::min(chunk_size, events.size() - i);
        chunks.emplace_back(events.begin() + static_cast<std::ptrdiff_t>(i),
                            events.begin() + static_cast<std::ptrdiff_t>(i + n));
    }

    for (int w = 0; w < warmup_runs; ++w) {
        for (const auto& c : chunks) {
            auto r = obce::ReferenceCodec::encode_stream(c, cfg);
            (void)r;
        }
    }

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(measured_runs) * chunks.size());
    for (int r = 0; r < measured_runs; ++r) {
        for (const auto& c : chunks) {
            const auto t0 = Clock::now();
            auto enc = obce::ReferenceCodec::encode_stream(c, cfg);
            const auto t1 = Clock::now();
            if (!enc.ok()) continue;
            const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            samples.push_back(ns / static_cast<double>(c.size()));
        }
    }
    return compute_stats(std::move(samples));
}

LatencyStats measure_reference_decode_latency(const std::vector<obce::MarketEvent>& events,
                                              const obce::CodecConfig& cfg,
                                              size_t chunk_size,
                                              int warmup_runs,
                                              int measured_runs) {
    std::vector<std::pair<std::vector<uint8_t>, size_t>> encoded_chunks;
    for (size_t i = 0; i < events.size(); i += chunk_size) {
        const size_t n = std::min(chunk_size, events.size() - i);
        std::vector<obce::MarketEvent> slice(events.begin() + static_cast<std::ptrdiff_t>(i),
                                             events.begin() + static_cast<std::ptrdiff_t>(i + n));
        auto enc = obce::ReferenceCodec::encode_stream(slice, cfg);
        encoded_chunks.emplace_back(std::move(*enc.value), n);
    }

    for (int w = 0; w < warmup_runs; ++w) {
        for (const auto& item : encoded_chunks) {
            auto r = obce::ReferenceCodec::decode_stream(item.first);
            (void)r;
        }
    }

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(measured_runs) * encoded_chunks.size());
    for (int r = 0; r < measured_runs; ++r) {
        for (const auto& item : encoded_chunks) {
            const auto t0 = Clock::now();
            auto dec = obce::ReferenceCodec::decode_stream(item.first);
            const auto t1 = Clock::now();
            if (!dec.ok()) continue;
            const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            samples.push_back(ns / static_cast<double>(item.second));
        }
    }
    return compute_stats(std::move(samples));
}

LatencyStats measure_delta_encode_latency(const std::vector<obce::MarketEvent>& events,
                                          const obce::CodecConfig& cfg,
                                          size_t chunk_size,
                                          int warmup_runs,
                                          int measured_runs) {
    std::vector<std::vector<obce::MarketEvent>> chunks;
    for (size_t i = 0; i < events.size(); i += chunk_size) {
        const size_t n = std::min(chunk_size, events.size() - i);
        chunks.emplace_back(events.begin() + static_cast<std::ptrdiff_t>(i),
                            events.begin() + static_cast<std::ptrdiff_t>(i + n));
    }

    for (int w = 0; w < warmup_runs; ++w) {
        for (const auto& c : chunks) {
            auto r = obce::DeltaEncoder::encode_all(c, cfg);
            (void)r;
        }
    }

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(measured_runs) * chunks.size());
    for (int r = 0; r < measured_runs; ++r) {
        for (const auto& c : chunks) {
            const auto t0 = Clock::now();
            auto enc = obce::DeltaEncoder::encode_all(c, cfg);
            const auto t1 = Clock::now();
            if (!enc.ok()) continue;
            const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            samples.push_back(ns / static_cast<double>(c.size()));
        }
    }
    return compute_stats(std::move(samples));
}

LatencyStats measure_delta_decode_latency(const std::vector<obce::MarketEvent>& events,
                                          const obce::CodecConfig& cfg,
                                          size_t chunk_size,
                                          int warmup_runs,
                                          int measured_runs) {
    std::vector<std::pair<std::vector<uint8_t>, size_t>> encoded_chunks;
    for (size_t i = 0; i < events.size(); i += chunk_size) {
        const size_t n = std::min(chunk_size, events.size() - i);
        std::vector<obce::MarketEvent> slice(events.begin() + static_cast<std::ptrdiff_t>(i),
                                             events.begin() + static_cast<std::ptrdiff_t>(i + n));
        auto enc = obce::DeltaEncoder::encode_all(slice, cfg);
        encoded_chunks.emplace_back(std::move(enc.value->bytes), n);
    }

    for (int w = 0; w < warmup_runs; ++w) {
        for (const auto& item : encoded_chunks) {
            auto r = obce::DeltaDecoder::decode_all(item.first);
            (void)r;
        }
    }

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(measured_runs) * encoded_chunks.size());
    for (int r = 0; r < measured_runs; ++r) {
        for (const auto& item : encoded_chunks) {
            const auto t0 = Clock::now();
            auto dec = obce::DeltaDecoder::decode_all(item.first);
            const auto t1 = Clock::now();
            if (!dec.ok()) continue;
            const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            samples.push_back(ns / static_cast<double>(item.second));
        }
    }
    return compute_stats(std::move(samples));
}

std::pair<LatencyStats, double> measure_random_seek(const std::vector<uint8_t>& bytes,
                                                    const std::vector<obce::MarketEvent>& events,
                                                    uint64_t seed,
                                                    size_t num_queries) {
    obce::StreamHeader hdr{};
    auto idx_res = obce::DeltaDecoder::read_checkpoint_index(bytes.data(), bytes.size(), &hdr);
    if (!idx_res.ok()) {
        return {LatencyStats{}, 0.0};
    }
    const auto& index = *idx_res.value;

    // Warm-up queries
    uint64_t rng = seed;
    for (size_t i = 0; i < 100; ++i) {
        const size_t idx = static_cast<size_t>(splitmix64(rng) % events.size());
        auto ev = obce::DeltaDecoder::seek_by_seq_id(bytes.data(), bytes.size(), events[idx].seq_id, index, hdr);
        (void)ev;
    }

    std::vector<double> samples;
    samples.reserve(num_queries);
    uint64_t total_scanned = 0;
    for (size_t i = 0; i < num_queries; ++i) {
        const size_t idx = static_cast<size_t>(splitmix64(rng) % events.size());
        obce::DiagnosticCounters diag{};
        const auto t0 = Clock::now();
        auto ev = obce::DeltaDecoder::seek_by_seq_id(bytes.data(), bytes.size(), events[idx].seq_id, index, hdr, &diag);
        const auto t1 = Clock::now();
        if (!ev.ok() || *ev.value != events[idx]) {
            std::cerr << "ERROR: Random seek mismatch at index " << idx << "\n";
            std::exit(1);
        }
        total_scanned += diag.events_processed;
        samples.push_back(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }

    return {compute_stats(std::move(samples)),
            static_cast<double>(total_scanned) / static_cast<double>(num_queries)};
}

std::string hex32(uint32_t v) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << v;
    return oss.str();
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_json = "reports/benchmark_results.json";
    std::string out_csv = "reports/benchmark_summary.csv";
    size_t event_count = 50000;

    for (int i = 1; i + 1 < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--json") out_json = argv[++i];
        else if (arg == "--csv") out_csv = argv[++i];
        else if (arg == "--events") event_count = std::stoull(argv[++i]);
    }

    const int warmup_runs = 2;
    const int measured_runs = 5;
    const size_t latency_chunk_size = 256;
    const size_t random_seek_queries = 1000;

    struct WorkloadSpec {
        obce::WorkloadProfile profile;
        uint64_t seed;
        std::vector<uint32_t> cp_intervals;
    };

    const std::vector<WorkloadSpec> specs = {
        {obce::WorkloadProfile::L3OrderFlow, 20261127ULL, {64, 256, 1024, 4096}},
        {obce::WorkloadProfile::L2TopOfBook, 20261128ULL, {256, 1024}},
        {obce::WorkloadProfile::VolatileBursty, 20261129ULL, {256, 1024}}
    };

    std::vector<BenchRow> rows;

    std::cout << "=== Order Book Compression Engine Benchmark Suite ===\n"
              << "Build: " << OBCE_BUILD_TYPE << " | Compiler: " << OBCE_CXX_COMPILER
              << " | Events/workload: " << event_count
              << " | Clock: std::chrono::steady_clock"
              << " | zstd: " << obce::ZstdAdapter::version_string() << "\n\n";

    for (const auto& spec : specs) {
        obce::GeneratorConfig gcfg{};
        gcfg.seed = spec.seed;
        gcfg.event_count = event_count;
        gcfg.num_symbols = 8;
        gcfg.schema_version = obce::kSchemaVersionV2;
        gcfg.profile = spec.profile;
        const auto events = obce::generate_synthetic_events(gcfg);
        const uint32_t in_digest = obce::compute_events_digest(events, obce::kSchemaVersionV2);

        // 1. Baseline Reference Binary Codec
        obce::CodecConfig ref_cfg{};
        ref_cfg.schema_version = obce::kSchemaVersionV2;
        ref_cfg.checkpoint_interval = 256;
        ref_cfg.delta_frame_events = 64;

        const auto t_ref_enc0 = Clock::now();
        auto ref_enc = obce::ReferenceCodec::encode_stream(events, ref_cfg);
        const auto t_ref_enc1 = Clock::now();
        const auto t_ref_dec0 = Clock::now();
        auto ref_dec = obce::ReferenceCodec::decode_stream(*ref_enc.value);
        const auto t_ref_dec1 = Clock::now();

        const double ref_enc_s = std::chrono::duration<double>(t_ref_enc1 - t_ref_enc0).count();
        const double ref_dec_s = std::chrono::duration<double>(t_ref_dec1 - t_ref_dec0).count();
        const size_t ref_bytes = ref_enc.value->size();

        BenchRow ref_row{};
        ref_row.workload = obce::workload_profile_name(spec.profile);
        ref_row.codec = "reference_binary_v2";
        ref_row.seed = spec.seed;
        ref_row.event_count = event_count;
        ref_row.checkpoint_interval = 0;
        ref_row.encoded_bytes = ref_bytes;
        ref_row.bytes_per_event = static_cast<double>(ref_bytes) / static_cast<double>(event_count);
        ref_row.compression_ratio = 1.0;
        ref_row.encode_per_event_ns = measure_reference_encode_latency(events, ref_cfg, latency_chunk_size, warmup_runs, measured_runs);
        ref_row.decode_per_event_ns = measure_reference_decode_latency(events, ref_cfg, latency_chunk_size, warmup_runs, measured_runs);
        ref_row.encode_mevents_per_sec = (static_cast<double>(event_count) / ref_enc_s) / 1e6;
        ref_row.decode_mevents_per_sec = (static_cast<double>(event_count) / ref_dec_s) / 1e6;
        ref_row.input_digest = in_digest;
        ref_row.decoded_digest = obce::compute_events_digest(*ref_dec.value, obce::kSchemaVersionV2);
        ref_row.lossless_verified = (ref_dec.ok() && *ref_dec.value == events);
        rows.push_back(ref_row);

        // 2. Delta + Varint + Checkpoints across checkpoint intervals
        for (uint32_t cp : spec.cp_intervals) {
            obce::CodecConfig dcfg{};
            dcfg.schema_version = obce::kSchemaVersionV2;
            dcfg.checkpoint_interval = cp;
            dcfg.delta_frame_events = 64;
            dcfg.write_index_footer = true;

            const auto t_enc0 = Clock::now();
            auto d_enc = obce::DeltaEncoder::encode_all(events, dcfg);
            const auto t_enc1 = Clock::now();
            const auto t_dec0 = Clock::now();
            auto d_dec = obce::DeltaDecoder::decode_all(d_enc.value->bytes, obce::GapPolicy::StrictReject);
            const auto t_dec1 = Clock::now();

            const double enc_s = std::chrono::duration<double>(t_enc1 - t_enc0).count();
            const double dec_s = std::chrono::duration<double>(t_dec1 - t_dec0).count();
            const size_t d_bytes = d_enc.value->bytes.size();

            BenchRow drow{};
            drow.workload = obce::workload_profile_name(spec.profile);
            drow.codec = "delta_varint_v2";
            drow.seed = spec.seed;
            drow.event_count = event_count;
            drow.checkpoint_interval = cp;
            drow.encoded_bytes = d_bytes;
            drow.bytes_per_event = static_cast<double>(d_bytes) / static_cast<double>(event_count);
            drow.compression_ratio = static_cast<double>(ref_bytes) / static_cast<double>(d_bytes);
            drow.encode_per_event_ns = measure_delta_encode_latency(events, dcfg, latency_chunk_size, warmup_runs, measured_runs);
            drow.decode_per_event_ns = measure_delta_decode_latency(events, dcfg, latency_chunk_size, warmup_runs, measured_runs);
            drow.encode_mevents_per_sec = (static_cast<double>(event_count) / enc_s) / 1e6;
            drow.decode_mevents_per_sec = (static_cast<double>(event_count) / dec_s) / 1e6;
            auto [seek_stats, mean_scanned] = measure_random_seek(d_enc.value->bytes, events, spec.seed ^ cp, random_seek_queries);
            drow.random_seek_ns = seek_stats;
            drow.random_seek_mean_events_scanned = mean_scanned;
            drow.input_digest = in_digest;
            drow.decoded_digest = obce::compute_events_digest(d_dec.value->events, obce::kSchemaVersionV2);
            drow.lossless_verified = (d_dec.ok() && d_dec.value->events == events);
            rows.push_back(drow);
        }
    }

    // Print console table
    std::cout << std::left << std::setw(16) << "Workload"
              << std::setw(20) << "Codec"
              << std::right << std::setw(6) << "CP"
              << std::setw(10) << "Bytes/Ev"
              << std::setw(8) << "Ratio"
              << std::setw(11) << "Enc p99ns"
              << std::setw(11) << "Dec p99ns"
              << std::setw(12) << "Seek p99ns"
              << std::setw(10) << "ScanEv"
              << "\n";
    std::cout << std::string(104, '-') << "\n";
    for (const auto& r : rows) {
        std::cout << std::left << std::setw(16) << r.workload
                  << std::setw(20) << r.codec
                  << std::right << std::setw(6) << r.checkpoint_interval
                  << std::fixed << std::setprecision(2)
                  << std::setw(10) << r.bytes_per_event
                  << std::setw(7) << r.compression_ratio << "x"
                  << std::setw(11) << r.encode_per_event_ns.p99_ns
                  << std::setw(11) << r.decode_per_event_ns.p99_ns
                  << std::setw(12) << r.random_seek_ns.p99_ns
                  << std::setw(10) << r.random_seek_mean_events_scanned
                  << "\n";
    }

    // Write CSV
    {
        std::ofstream csv(out_csv, std::ios::trunc);
        csv << "workload,codec,seed,event_count,checkpoint_interval,encoded_bytes,bytes_per_event,"
            << "compression_ratio,encode_p50_ns,encode_p99_ns,decode_p50_ns,decode_p99_ns,"
            << "encode_mevents_sec,decode_mevents_sec,random_seek_p50_ns,random_seek_p99_ns,"
            << "random_seek_mean_events_scanned,lossless_verified\n";
        csv << std::fixed << std::setprecision(3);
        for (const auto& r : rows) {
            csv << r.workload << "," << r.codec << "," << r.seed << "," << r.event_count << ","
                << r.checkpoint_interval << "," << r.encoded_bytes << "," << r.bytes_per_event << ","
                << r.compression_ratio << "," << r.encode_per_event_ns.p50_ns << ","
                << r.encode_per_event_ns.p99_ns << "," << r.decode_per_event_ns.p50_ns << ","
                << r.decode_per_event_ns.p99_ns << "," << r.encode_mevents_per_sec << ","
                << r.decode_mevents_per_sec << "," << r.random_seek_ns.p50_ns << ","
                << r.random_seek_ns.p99_ns << "," << r.random_seek_mean_events_scanned << ","
                << (r.lossless_verified ? "true" : "false") << "\n";
        }
    }

    // Write JSON
    {
        std::ofstream js(out_json, std::ios::trunc);
        js << std::fixed << std::setprecision(3);
        js << "{\n"
           << "  \"metadata\": {\n"
           << "    \"project\": \"order-book-compression-engine\",\n"
           << "    \"schema_version\": 2,\n"
           << "    \"build_type\": \"" << OBCE_BUILD_TYPE << "\",\n"
           << "    \"compiler\": \"" << OBCE_CXX_COMPILER << "\",\n"
           << "    \"clock_source\": \"std::chrono::steady_clock\",\n"
           << "    \"events_per_workload\": " << event_count << ",\n"
           << "    \"latency_chunk_events\": " << latency_chunk_size << ",\n"
           << "    \"warmup_runs\": " << warmup_runs << ",\n"
           << "    \"measured_runs\": " << measured_runs << ",\n"
           << "    \"random_seek_queries\": " << random_seek_queries << ",\n"
           << "    \"zstd_available\": " << (obce::ZstdAdapter::is_available() ? "true" : "false") << ",\n"
           << "    \"zstd_version\": \"" << obce::ZstdAdapter::version_string() << "\",\n"
           << "    \"data_provenance\": \"synthetic deterministic generator (SplitMix64/Xoshiro256**)\"\n"
           << "  },\n"
           << "  \"results\": [\n";
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& r = rows[i];
            js << "    {\n"
               << "      \"workload\": \"" << r.workload << "\",\n"
               << "      \"codec\": \"" << r.codec << "\",\n"
               << "      \"seed\": " << r.seed << ",\n"
               << "      \"event_count\": " << r.event_count << ",\n"
               << "      \"checkpoint_interval\": " << r.checkpoint_interval << ",\n"
               << "      \"encoded_bytes\": " << r.encoded_bytes << ",\n"
               << "      \"bytes_per_event\": " << r.bytes_per_event << ",\n"
               << "      \"compression_ratio_vs_reference\": " << r.compression_ratio << ",\n"
               << "      \"encode_ns_per_event\": {"
               << "\"min\":" << r.encode_per_event_ns.min_ns
               << ",\"p50\":" << r.encode_per_event_ns.p50_ns
               << ",\"p95\":" << r.encode_per_event_ns.p95_ns
               << ",\"p99\":" << r.encode_per_event_ns.p99_ns
               << ",\"p999\":" << r.encode_per_event_ns.p999_ns
               << ",\"mean\":" << r.encode_per_event_ns.mean_ns << "},\n"
               << "      \"decode_ns_per_event\": {"
               << "\"min\":" << r.decode_per_event_ns.min_ns
               << ",\"p50\":" << r.decode_per_event_ns.p50_ns
               << ",\"p95\":" << r.decode_per_event_ns.p95_ns
               << ",\"p99\":" << r.decode_per_event_ns.p99_ns
               << ",\"p999\":" << r.decode_per_event_ns.p999_ns
               << ",\"mean\":" << r.decode_per_event_ns.mean_ns << "},\n"
               << "      \"encode_mevents_per_sec\": " << r.encode_mevents_per_sec << ",\n"
               << "      \"decode_mevents_per_sec\": " << r.decode_mevents_per_sec << ",\n"
               << "      \"random_seek_ns\": {"
               << "\"min\":" << r.random_seek_ns.min_ns
               << ",\"p50\":" << r.random_seek_ns.p50_ns
               << ",\"p95\":" << r.random_seek_ns.p95_ns
               << ",\"p99\":" << r.random_seek_ns.p99_ns
               << ",\"p999\":" << r.random_seek_ns.p999_ns
               << ",\"mean\":" << r.random_seek_ns.mean_ns << "},\n"
               << "      \"random_seek_mean_events_scanned\": " << r.random_seek_mean_events_scanned << ",\n"
               << "      \"input_digest\": \"" << hex32(r.input_digest) << "\",\n"
               << "      \"decoded_digest\": \"" << hex32(r.decoded_digest) << "\",\n"
               << "      \"lossless_verified\": " << (r.lossless_verified ? "true" : "false") << "\n"
               << "    }" << (i + 1 < rows.size() ? "," : "") << "\n";
        }
        js << "  ]\n}\n";
    }

    std::cout << "\nWrote raw reports:\n  JSON: " << out_json << "\n  CSV:  " << out_csv << "\n";
    return 0;
}

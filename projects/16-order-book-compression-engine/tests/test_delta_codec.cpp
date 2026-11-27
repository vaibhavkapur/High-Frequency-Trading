#include <cstdint>
#include <vector>

#include "obce/delta_codec.hpp"
#include "obce/fixture_generator.hpp"
#include "obce/reference_codec.hpp"
#include "test_harness.hpp"

namespace {

// Deterministic SplitMix64 for repeatable fuzzing
uint64_t next_fuzz(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

}  // namespace

int main() {
    obce_test::register_test("delta_roundtrip_all_workloads_and_checkpoints", []() {
        const std::vector<obce::WorkloadProfile> profiles = {
            obce::WorkloadProfile::L3OrderFlow,
            obce::WorkloadProfile::L2TopOfBook,
            obce::WorkloadProfile::VolatileBursty
        };
        const std::vector<uint32_t> cp_intervals = {1, 16, 64, 256};

        for (auto profile : profiles) {
            obce::GeneratorConfig gcfg{};
            gcfg.seed = 1001 + static_cast<uint64_t>(profile);
            gcfg.event_count = 1500;
            gcfg.profile = profile;
            gcfg.schema_version = obce::kSchemaVersionV2;
            const auto events = obce::generate_synthetic_events(gcfg);

            for (uint32_t cp : cp_intervals) {
                obce::CodecConfig ccfg{};
                ccfg.schema_version = obce::kSchemaVersionV2;
                ccfg.checkpoint_interval = cp;
                ccfg.delta_frame_events = 32;
                ccfg.write_index_footer = true;

                auto enc = obce::DeltaEncoder::encode_all(events, ccfg);
                OBCE_ASSERT_TRUE(enc.ok());
                OBCE_ASSERT_TRUE(!enc.value->checkpoints.empty());

                auto dec = obce::DeltaDecoder::decode_all(enc.value->bytes, obce::GapPolicy::StrictReject);
                OBCE_ASSERT_TRUE(dec.ok());
                OBCE_ASSERT_EQ(dec.value->events.size(), events.size());
                OBCE_ASSERT_EQ(dec.value->events, events);
            }
        }
    });

    obce_test::register_test("delta_compression_smaller_than_reference_on_l3_and_l2", []() {
        obce::GeneratorConfig gcfg{};
        gcfg.seed = 42;
        gcfg.event_count = 5000;
        gcfg.profile = obce::WorkloadProfile::L3OrderFlow;
        const auto l3 = obce::generate_synthetic_events(gcfg);

        obce::CodecConfig cfg{};
        cfg.checkpoint_interval = 256;
        cfg.delta_frame_events = 64;

        auto ref = obce::ReferenceCodec::encode_stream(l3, cfg);
        auto del = obce::DeltaEncoder::encode_all(l3, cfg);
        OBCE_ASSERT_TRUE(ref.ok() && del.ok());
        // Delta+varint must achieve substantial compression (>2.5x) on realistic L3 order flow
        OBCE_ASSERT_TRUE(del.value->bytes.size() * 25 < ref.value->size() * 10);
    });

    obce_test::register_test("random_access_seek_and_range_decode", []() {
        obce::GeneratorConfig gcfg{};
        gcfg.seed = 777;
        gcfg.event_count = 2000;
        gcfg.start_seq_id = 5000;
        gcfg.profile = obce::WorkloadProfile::L3OrderFlow;
        const auto events = obce::generate_synthetic_events(gcfg);

        obce::CodecConfig cfg{};
        cfg.checkpoint_interval = 128;
        cfg.delta_frame_events = 32;
        cfg.write_index_footer = true;

        auto enc = obce::DeltaEncoder::encode_all(events, cfg);
        OBCE_ASSERT_TRUE(enc.ok());

        obce::StreamHeader hdr{};
        auto idx = obce::DeltaDecoder::read_checkpoint_index(enc.value->bytes.data(), enc.value->bytes.size(), &hdr);
        OBCE_ASSERT_TRUE(idx.ok());
        OBCE_ASSERT_EQ(idx.value->size(), enc.value->checkpoints.size());

        // Test seeks across beginning, checkpoint boundaries, mid-delta frames, and end
        const std::vector<size_t> probe_indices = {0, 1, 127, 128, 129, 511, 1000, 1998, 1999};
        for (size_t i : probe_indices) {
            obce::DiagnosticCounters diag{};
            auto res = obce::DeltaDecoder::seek_by_seq_id(
                enc.value->bytes.data(), enc.value->bytes.size(),
                events[i].seq_id, *idx.value, hdr, &diag);
            OBCE_ASSERT_TRUE(res.ok());
            OBCE_ASSERT_EQ(*res.value, events[i]);
            // Bounded scan cost: never decodes more than checkpoint_interval events
            OBCE_ASSERT_TRUE(diag.events_processed <= cfg.checkpoint_interval);
        }

        // Range decode spanning multiple checkpoints
        auto range = obce::DeltaDecoder::decode_seq_range(
            enc.value->bytes.data(), enc.value->bytes.size(),
            events[100].seq_id, events[350].seq_id, *idx.value, hdr);
        OBCE_ASSERT_TRUE(range.ok());
        OBCE_ASSERT_EQ(range.value->size(), 251U);
        for (size_t i = 0; i < range.value->size(); ++i) {
            OBCE_ASSERT_EQ((*range.value)[i], events[100 + i]);
        }

        // Fallback index scan without IndexFooter
        cfg.write_index_footer = false;
        auto enc_no_footer = obce::DeltaEncoder::encode_all(events, cfg);
        OBCE_ASSERT_TRUE(enc_no_footer.ok());
        auto idx2 = obce::DeltaDecoder::read_checkpoint_index(
            enc_no_footer.value->bytes.data(), enc_no_footer.value->bytes.size(), &hdr);
        OBCE_ASSERT_TRUE(idx2.ok());
        OBCE_ASSERT_EQ(idx2.value->size(), enc_no_footer.value->checkpoints.size());
    });

    obce_test::register_test("fuzzed_malformed_payloads_rejected_safely", []() {
        obce::GeneratorConfig gcfg{};
        gcfg.seed = 999;
        gcfg.event_count = 300;
        const auto events = obce::generate_synthetic_events(gcfg);
        obce::CodecConfig cfg{};
        cfg.checkpoint_interval = 64;
        cfg.delta_frame_events = 32;
        auto enc = obce::DeltaEncoder::encode_all(events, cfg);
        OBCE_ASSERT_TRUE(enc.ok());
        const auto& good = enc.value->bytes;

        uint64_t rng = 0xDEADBEEFCAFEBABEULL;

        // 1. 250 single-bit and multi-byte mutations across the encoded stream
        for (int iter = 0; iter < 250; ++iter) {
            std::vector<uint8_t> mutated = good;
            const size_t pos = static_cast<size_t>(next_fuzz(rng) % mutated.size());
            const uint8_t mask = static_cast<uint8_t>(1U << (next_fuzz(rng) % 8U));
            mutated[pos] ^= mask;
            auto dec = obce::DeltaDecoder::decode_all(mutated, obce::GapPolicy::StrictReject);
            // If mutation hit the EOF index trailer only, frame decode might still succeed,
            // otherwise it MUST fail cleanly without crashing!
            if (pos < good.size() - 12 && pos >= obce::kStreamHeaderSize) {
                OBCE_ASSERT_TRUE(!dec.ok());
            }
        }

        // 2. 200 pure random garbage buffers of sizes 0..512
        for (int iter = 0; iter < 200; ++iter) {
            const size_t sz = static_cast<size_t>(next_fuzz(rng) % 512U);
            std::vector<uint8_t> garbage(sz);
            for (size_t i = 0; i < sz; ++i) {
                garbage[i] = static_cast<uint8_t>(next_fuzz(rng) & 0xFFU);
            }
            auto dec = obce::DeltaDecoder::decode_all(garbage, obce::GapPolicy::StrictReject);
            OBCE_ASSERT_TRUE(!dec.ok());
            auto dec_resync = obce::DeltaDecoder::decode_all(garbage, obce::GapPolicy::ResyncAtCheckpoint);
            OBCE_ASSERT_TRUE(!dec_resync.ok());
        }
    });

    return obce_test::run_all_tests("test_delta_codec");
}

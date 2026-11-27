#include <climits>
#include <cstdint>
#include <vector>

#include "obce/crc32.hpp"
#include "obce/delta_codec.hpp"
#include "obce/fixture_generator.hpp"
#include "obce/reference_codec.hpp"
#include "test_harness.hpp"

int main() {
    obce_test::register_test("extreme_deltas_and_wrap_safe_overflow_roundtrip", []() {
        obce::GeneratorConfig gcfg{};
        gcfg.seed = 12345;
        gcfg.event_count = 512;
        gcfg.profile = obce::WorkloadProfile::ExtremeDeltas;
        gcfg.schema_version = obce::kSchemaVersionV2;
        const auto events = obce::generate_synthetic_events(gcfg);

        // Verify extreme values actually exist in generated stream
        OBCE_ASSERT_EQ(events[0].price_ticks, INT64_MIN);
        OBCE_ASSERT_EQ(events[1].price_ticks, INT64_MAX);
        OBCE_ASSERT_EQ(events[1].quantity, INT64_MAX);
        OBCE_ASSERT_EQ(events[1].order_id, UINT64_MAX);
        OBCE_ASSERT_EQ(events[1].symbol_id, UINT32_MAX);

        obce::CodecConfig cfg{};
        cfg.schema_version = obce::kSchemaVersionV2;
        cfg.checkpoint_interval = 64;
        cfg.delta_frame_events = 16;

        auto ref_enc = obce::ReferenceCodec::encode_stream(events, cfg);
        OBCE_ASSERT_TRUE(ref_enc.ok());
        auto ref_dec = obce::ReferenceCodec::decode_stream(*ref_enc.value);
        OBCE_ASSERT_TRUE(ref_dec.ok());
        OBCE_ASSERT_EQ(*ref_dec.value, events);

        auto del_enc = obce::DeltaEncoder::encode_all(events, cfg);
        OBCE_ASSERT_TRUE(del_enc.ok());
        auto del_dec = obce::DeltaDecoder::decode_all(del_enc.value->bytes, obce::GapPolicy::StrictReject);
        OBCE_ASSERT_TRUE(del_dec.ok());
        OBCE_ASSERT_EQ(del_dec.value->events, events);
    });

    obce_test::register_test("truncation_at_every_byte_offset_handled", []() {
        obce::GeneratorConfig gcfg{};
        gcfg.seed = 314159;
        gcfg.event_count = 150;
        gcfg.profile = obce::WorkloadProfile::L3OrderFlow;
        const auto events = obce::generate_synthetic_events(gcfg);

        obce::CodecConfig cfg{};
        cfg.checkpoint_interval = 50;
        cfg.delta_frame_events = 25;
        cfg.write_index_footer = false;  // Every non-frame-boundary cut is strictly a truncation

        auto enc = obce::DeltaEncoder::encode_all(events, cfg);
        OBCE_ASSERT_TRUE(enc.ok());
        const auto& bytes = enc.value->bytes;

        // Track exact valid frame end boundaries (stream header + complete frames)
        std::vector<size_t> valid_boundaries;
        size_t off = obce::kStreamHeaderSize;
        valid_boundaries.push_back(off);  // Header-only empty stream
        while (off < bytes.size()) {
            const uint32_t plen = obce::read_u32_le(bytes.data() + off + 4);
            off += obce::kFrameHeaderSize + plen + obce::kFrameCrcSize;
            valid_boundaries.push_back(off);
        }

        for (size_t cut = 0; cut < bytes.size(); ++cut) {
            bool is_boundary = false;
            for (size_t b : valid_boundaries) {
                if (b == cut) { is_boundary = true; break; }
            }
            auto dec = obce::DeltaDecoder::decode_all(bytes.data(), cut, obce::GapPolicy::StrictReject);
            if (is_boundary) {
                OBCE_ASSERT_TRUE(dec.ok());
            } else {
                OBCE_ASSERT_TRUE(!dec.ok());
                OBCE_ASSERT_EQ(dec.status.code, obce::ErrorCode::TruncatedInput);
            }
        }
    });

    obce_test::register_test("sequence_gap_policies_strict_allow_and_resync", []() {
        obce::GeneratorConfig gcfg{};
        gcfg.seed = 2026;
        gcfg.event_count = 100;
        auto events = obce::generate_synthetic_events(gcfg);

        // Introduce an explicit sequence gap of 10 missing events at index 40
        for (size_t i = 40; i < events.size(); ++i) {
            events[i].seq_id += 10ULL;
        }

        // StrictReject must reject on encode
        obce::CodecConfig strict_cfg{};
        strict_cfg.gap_policy = obce::GapPolicy::StrictReject;
        auto enc_strict = obce::DeltaEncoder::encode_all(events, strict_cfg);
        OBCE_ASSERT_TRUE(!enc_strict.ok());
        OBCE_ASSERT_EQ(enc_strict.status.code, obce::ErrorCode::SequenceGap);

        // AllowAndFlag encodes the gap cleanly and flags diagnostics
        obce::CodecConfig allow_cfg{};
        allow_cfg.gap_policy = obce::GapPolicy::AllowAndFlag;
        allow_cfg.checkpoint_interval = 64;
        allow_cfg.delta_frame_events = 32;
        auto enc_allow = obce::DeltaEncoder::encode_all(events, allow_cfg);
        OBCE_ASSERT_TRUE(enc_allow.ok());
        OBCE_ASSERT_EQ(enc_allow.value->counters.sequence_gaps_detected, 1U);
        OBCE_ASSERT_EQ(enc_allow.value->counters.missing_events_estimated, 10U);

        // Decoding that gap stream with StrictReject must reject
        auto dec_strict = obce::DeltaDecoder::decode_all(enc_allow.value->bytes, obce::GapPolicy::StrictReject);
        OBCE_ASSERT_TRUE(!dec_strict.ok());
        OBCE_ASSERT_EQ(dec_strict.status.code, obce::ErrorCode::SequenceGap);

        // Decoding with AllowAndFlag succeeds losslessly and reports gap count
        auto dec_allow = obce::DeltaDecoder::decode_all(enc_allow.value->bytes, obce::GapPolicy::AllowAndFlag);
        OBCE_ASSERT_TRUE(dec_allow.ok());
        OBCE_ASSERT_EQ(dec_allow.value->events, events);
        OBCE_ASSERT_EQ(dec_allow.value->counters.sequence_gaps_detected, 1U);
        OBCE_ASSERT_EQ(dec_allow.value->counters.missing_events_estimated, 10U);

        // Sequence regression must ALWAYS fail regardless of policy
        auto reg_events = events;
        reg_events[50].seq_id = reg_events[49].seq_id;
        auto enc_reg = obce::DeltaEncoder::encode_all(reg_events, allow_cfg);
        OBCE_ASSERT_TRUE(!enc_reg.ok());
        OBCE_ASSERT_EQ(enc_reg.status.code, obce::ErrorCode::SequenceRegression);
    });

    obce_test::register_test("recovery_from_missing_delta_frame", []() {
        // Build a stream with 3 checkpoints (interval=64, delta_frame_events=16) -> 192 events total
        // Checkpoint 0: event 0 (seq 1)
        //   Delta 0a: events 1..16 (seq 2..17)
        //   Delta 0b: events 17..32 (seq 18..33)   <-- WE WILL PHYSICALLY DELETE THIS DELTA FRAME!
        //   Delta 0c: events 33..48 (seq 34..49)
        //   Delta 0d: events 49..63 (seq 50..64)
        // Checkpoint 1: event 64 (seq 65)
        //   Deltas 1a..1d: events 65..127 (seq 66..128)
        // Checkpoint 2: event 128 (seq 129)
        //   Deltas 2a..2d: events 129..191 (seq 130..192)
        obce::GeneratorConfig gcfg{};
        gcfg.seed = 8080;
        gcfg.event_count = 192;
        gcfg.profile = obce::WorkloadProfile::L3OrderFlow;
        const auto events = obce::generate_synthetic_events(gcfg);

        obce::CodecConfig cfg{};
        cfg.checkpoint_interval = 64;
        cfg.delta_frame_events = 16;
        cfg.write_index_footer = false;

        auto enc = obce::DeltaEncoder::encode_all(events, cfg);
        OBCE_ASSERT_TRUE(enc.ok());
        const auto& good = enc.value->bytes;

        // Parse frame byte spans
        struct FrameSpan { size_t start; size_t len; uint8_t type; };
        std::vector<FrameSpan> spans;
        size_t off = obce::kStreamHeaderSize;
        while (off < good.size()) {
            const uint8_t ftype = good[off + 2];
            const uint32_t plen = obce::read_u32_le(good.data() + off + 4);
            const size_t total = obce::kFrameHeaderSize + plen + obce::kFrameCrcSize;
            spans.push_back(FrameSpan{off, total, ftype});
            off += total;
        }
        OBCE_ASSERT_TRUE(spans.size() >= 6);
        OBCE_ASSERT_EQ(spans[0].type, static_cast<uint8_t>(obce::FrameType::Checkpoint));
        OBCE_ASSERT_EQ(spans[1].type, static_cast<uint8_t>(obce::FrameType::Delta));
        OBCE_ASSERT_EQ(spans[2].type, static_cast<uint8_t>(obce::FrameType::Delta));

        // Construct wire stream with spans[2] (Delta 0b) completely missing!
        std::vector<uint8_t> missing_delta_stream;
        missing_delta_stream.insert(missing_delta_stream.end(), good.begin(), good.begin() + obce::kStreamHeaderSize);
        for (size_t i = 0; i < spans.size(); ++i) {
            if (i == 2) continue;  // Drop Delta 0b!
            missing_delta_stream.insert(missing_delta_stream.end(),
                                        good.begin() + static_cast<std::ptrdiff_t>(spans[i].start),
                                        good.begin() + static_cast<std::ptrdiff_t>(spans[i].start + spans[i].len));
        }

        // StrictReject MUST fail explicitly with SequenceGap (never silently decode garbage deltas!)
        auto strict_dec = obce::DeltaDecoder::decode_all(missing_delta_stream, obce::GapPolicy::StrictReject);
        OBCE_ASSERT_TRUE(!strict_dec.ok());
        OBCE_ASSERT_EQ(strict_dec.status.code, obce::ErrorCode::SequenceGap);

        // ResyncAtCheckpoint MUST recover cleanly at Checkpoint 1 (index 64) and decode 0..16 + 64..191 identically!
        auto resync_dec = obce::DeltaDecoder::decode_all(missing_delta_stream, obce::GapPolicy::ResyncAtCheckpoint);
        OBCE_ASSERT_TRUE(resync_dec.ok());
        OBCE_ASSERT_EQ(resync_dec.value->counters.resync_recoveries, 1U);
        OBCE_ASSERT_TRUE(resync_dec.value->counters.corrupt_frames_skipped >= 1U);
        OBCE_ASSERT_TRUE(resync_dec.value->counters.sequence_gaps_detected >= 1U);

        // Expected recovered events: [0..16] (17 events) + [64..191] (128 events) = 145 exact events!
        OBCE_ASSERT_EQ(resync_dec.value->events.size(), 17U + 128U);
        for (size_t i = 0; i < 17; ++i) {
            OBCE_ASSERT_EQ(resync_dec.value->events[i], events[i]);
        }
        for (size_t i = 0; i < 128; ++i) {
            OBCE_ASSERT_EQ(resync_dec.value->events[17 + i], events[64 + i]);
        }
    });

    obce_test::register_test("schema_evolution_v1_to_v2_and_unsupported_version", []() {
        obce::GeneratorConfig gcfg{};
        gcfg.seed = 555;
        gcfg.event_count = 120;
        gcfg.schema_version = obce::kSchemaVersionV1;
        gcfg.profile = obce::WorkloadProfile::L3OrderFlow;
        const auto v1_events = obce::generate_synthetic_events(gcfg);

        // Encode with Schema v1
        obce::CodecConfig cfg_v1{};
        cfg_v1.schema_version = obce::kSchemaVersionV1;
        cfg_v1.checkpoint_interval = 40;
        cfg_v1.delta_frame_events = 20;
        auto enc_v1 = obce::DeltaEncoder::encode_all(v1_events, cfg_v1);
        OBCE_ASSERT_TRUE(enc_v1.ok());

        // Decode v1 stream with current v2-capable decoder -> lossless upgrade, v2 fields == 0
        auto dec_v1 = obce::DeltaDecoder::decode_all(enc_v1.value->bytes, obce::GapPolicy::StrictReject);
        OBCE_ASSERT_TRUE(dec_v1.ok());
        OBCE_ASSERT_EQ(dec_v1.value->header.schema_version, obce::kSchemaVersionV1);
        OBCE_ASSERT_EQ(dec_v1.value->counters.schema_v1_upgrades, 120U);
        OBCE_ASSERT_EQ(dec_v1.value->events, v1_events);
        for (const auto& ev : dec_v1.value->events) {
            OBCE_ASSERT_EQ(ev.venue_id, 0U);
            OBCE_ASSERT_EQ(ev.flags, 0U);
            OBCE_ASSERT_EQ(ev.match_id, 0ULL);
        }

        // Tamper stream header schema_version to v99 -> must reject explicitly
        std::vector<uint8_t> future_stream = enc_v1.value->bytes;
        obce::write_u16_le(future_stream.data() + 4, 99U);
        auto dec_v99 = obce::DeltaDecoder::decode_all(future_stream, obce::GapPolicy::StrictReject);
        OBCE_ASSERT_TRUE(!dec_v99.ok());
        OBCE_ASSERT_EQ(dec_v99.status.code, obce::ErrorCode::UnsupportedSchemaVersion);
    });

    return obce_test::run_all_tests("test_integration_resilience");
}

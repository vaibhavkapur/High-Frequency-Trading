#include <fstream>
#include <string>
#include <vector>

#include "obce/io_adapter.hpp"
#include "obce/reference_codec.hpp"
#include "test_harness.hpp"

int main(int argc, char** argv) {
    std::string fixtures_dir = "fixtures";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--fixtures") {
            fixtures_dir = argv[i + 1];
        }
    }

    obce_test::register_test("reference_v1_fixture_roundtrip_and_size", [&]() {
        auto ev_res = obce::load_events_jsonl(fixtures_dir + "/valid_l2_l3_v1.jsonl");
        OBCE_ASSERT_TRUE(ev_res.ok());
        OBCE_ASSERT_EQ(ev_res.value->size(), 8U);

        obce::CodecConfig cfg{};
        cfg.schema_version = obce::kSchemaVersionV1;
        cfg.delta_frame_events = 4;

        obce::DiagnosticCounters enc_diag{};
        auto encoded = obce::ReferenceCodec::encode_stream(*ev_res.value, cfg, &enc_diag);
        OBCE_ASSERT_TRUE(encoded.ok());
        // Expected exact byte count: 24 (header) + 2 frames * (20 header + 4 * 47 payload + 4 CRC)
        const size_t expected_bytes = obce::kStreamHeaderSize + 2U * (obce::kFrameHeaderSize + 4U * obce::kRefRecordSizeV1 + obce::kFrameCrcSize);
        OBCE_ASSERT_EQ(encoded.value->size(), expected_bytes);
        OBCE_ASSERT_EQ(enc_diag.frames_processed, 2U);
        OBCE_ASSERT_EQ(enc_diag.events_processed, 8U);

        obce::DiagnosticCounters dec_diag{};
        obce::StreamHeader hdr{};
        auto decoded = obce::ReferenceCodec::decode_stream(*encoded.value, obce::GapPolicy::StrictReject, &dec_diag, &hdr);
        OBCE_ASSERT_TRUE(decoded.ok());
        OBCE_ASSERT_EQ(hdr.schema_version, obce::kSchemaVersionV1);
        OBCE_ASSERT_EQ(*decoded.value, *ev_res.value);
        OBCE_ASSERT_EQ(dec_diag.schema_v1_upgrades, 8U);
    });

    obce_test::register_test("reference_v2_fixture_roundtrip_and_negative_price", [&]() {
        auto ev_res = obce::load_events_jsonl(fixtures_dir + "/valid_l2_l3_v2.jsonl");
        OBCE_ASSERT_TRUE(ev_res.ok());
        OBCE_ASSERT_EQ(ev_res.value->size(), 5U);
        // Verify negative spread/yield price_ticks preserved
        OBCE_ASSERT_EQ((*ev_res.value)[3].price_ticks, -50000000LL);

        obce::CodecConfig cfg{};
        cfg.schema_version = obce::kSchemaVersionV2;
        cfg.delta_frame_events = 64;

        auto encoded = obce::ReferenceCodec::encode_stream(*ev_res.value, cfg);
        OBCE_ASSERT_TRUE(encoded.ok());
        const size_t expected_bytes = obce::kStreamHeaderSize + obce::kFrameHeaderSize + 5U * obce::kRefRecordSizeV2 + obce::kFrameCrcSize;
        OBCE_ASSERT_EQ(encoded.value->size(), expected_bytes);

        auto decoded = obce::ReferenceCodec::decode_stream(*encoded.value);
        OBCE_ASSERT_TRUE(decoded.ok());
        OBCE_ASSERT_EQ(*decoded.value, *ev_res.value);
    });

    obce_test::register_test("invalid_events_fixture_all_rejected", [&]() {
        std::ifstream ifs(fixtures_dir + "/invalid_events.jsonl");
        OBCE_ASSERT_TRUE(ifs.is_open());
        std::string line;
        int checked = 0;
        while (std::getline(ifs, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto parsed = obce::parse_event_json_line(line);
            OBCE_ASSERT_TRUE(parsed.ok());
            const auto st = obce::validate_event(*parsed.value, obce::kSchemaVersionV2);
            OBCE_ASSERT_TRUE(!st.ok());
            OBCE_ASSERT_EQ(st.code, obce::ErrorCode::InvalidEvent);
            ++checked;
        }
        OBCE_ASSERT_EQ(checked, 7);
    });

    obce_test::register_test("reference_crc_corruption_rejected", [&]() {
        auto ev_res = obce::load_events_jsonl(fixtures_dir + "/valid_l2_l3_v2.jsonl");
        OBCE_ASSERT_TRUE(ev_res.ok());
        obce::CodecConfig cfg{};
        auto encoded = obce::ReferenceCodec::encode_stream(*ev_res.value, cfg);
        OBCE_ASSERT_TRUE(encoded.ok());

        // Flip every bit in a payload byte and confirm CRC rejection
        std::vector<uint8_t> corrupt = *encoded.value;
        corrupt[obce::kStreamHeaderSize + obce::kFrameHeaderSize + 10] ^= 0x01U;
        obce::DiagnosticCounters diag{};
        auto dec = obce::ReferenceCodec::decode_stream(corrupt, obce::GapPolicy::StrictReject, &diag);
        OBCE_ASSERT_TRUE(!dec.ok());
        OBCE_ASSERT_EQ(dec.status.code, obce::ErrorCode::CrcMismatch);
        OBCE_ASSERT_EQ(diag.crc_errors, 1U);
    });

    return obce_test::run_all_tests("test_reference_codec");
}

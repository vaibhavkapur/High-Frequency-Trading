#include <string>
#include <vector>

#include "obce/delta_codec.hpp"
#include "obce/fixture_generator.hpp"
#include "obce/io_adapter.hpp"
#include "obce/reference_codec.hpp"
#include "obce/zstd_adapter.hpp"
#include "test_harness.hpp"

int main(int argc, char** argv) {
    std::string fixtures_dir = "fixtures";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--fixtures") {
            fixtures_dir = argv[i + 1];
        }
    }

    obce_test::register_test("reproducibility_byte_identical_across_reruns", []() {
        obce::GeneratorConfig gcfg{};
        gcfg.seed = 20261127ULL;
        gcfg.event_count = 1000;
        gcfg.profile = obce::WorkloadProfile::L3OrderFlow;

        obce::CodecConfig ccfg{};
        ccfg.schema_version = obce::kSchemaVersionV2;
        ccfg.checkpoint_interval = 128;
        ccfg.delta_frame_events = 32;
        ccfg.stream_id = 42;

        const auto ev1 = obce::generate_synthetic_events(gcfg);
        const auto ev2 = obce::generate_synthetic_events(gcfg);
        OBCE_ASSERT_EQ(ev1, ev2);

        auto enc1 = obce::DeltaEncoder::encode_all(ev1, ccfg);
        auto enc2 = obce::DeltaEncoder::encode_all(ev2, ccfg);
        OBCE_ASSERT_TRUE(enc1.ok() && enc2.ok());
        OBCE_ASSERT_EQ(enc1.value->bytes, enc2.value->bytes);

        const uint32_t d1 = obce::compute_events_digest(ev1, ccfg.schema_version);
        const uint32_t d2 = obce::compute_events_digest(ev2, ccfg.schema_version);
        OBCE_ASSERT_EQ(d1, d2);
    });

    obce_test::register_test("invalid_configuration_rejected_explicitly", []() {
        obce::CodecConfig bad1{};
        bad1.checkpoint_interval = 0;
        OBCE_ASSERT_EQ(obce::validate_config(bad1).code, obce::ErrorCode::InvalidArgument);

        obce::CodecConfig bad2{};
        bad2.price_decimals = 19;
        OBCE_ASSERT_EQ(obce::validate_config(bad2).code, obce::ErrorCode::InvalidArgument);

        obce::CodecConfig bad3{};
        bad3.delta_frame_events = 0;
        OBCE_ASSERT_EQ(obce::validate_config(bad3).code, obce::ErrorCode::InvalidArgument);

        obce::CodecConfig bad4{};
        bad4.schema_version = 3;
        OBCE_ASSERT_EQ(obce::validate_config(bad4).code, obce::ErrorCode::UnsupportedSchemaVersion);

        // Encoding with invalid config must fail immediately with no silent success
        auto enc = obce::DeltaEncoder::encode_all({}, bad1);
        OBCE_ASSERT_TRUE(!enc.ok());
        OBCE_ASSERT_EQ(enc.status.code, obce::ErrorCode::InvalidArgument);
    });

    obce_test::register_test("missing_files_and_unsupported_features_diagnostics", [&]() {
        auto missing = obce::read_binary_file(fixtures_dir + "/does_not_exist_12345.obce");
        OBCE_ASSERT_TRUE(!missing.ok());
        OBCE_ASSERT_EQ(missing.status.code, obce::ErrorCode::IoError);

        auto missing_jsonl = obce::load_events_jsonl(fixtures_dir + "/does_not_exist_12345.jsonl");
        OBCE_ASSERT_TRUE(!missing_jsonl.ok());
        OBCE_ASSERT_EQ(missing_jsonl.status.code, obce::ErrorCode::IoError);

        // Optional zstd adapter: if not compiled in, must explicitly report UnsupportedFeature
        const uint8_t sample[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        if (!obce::ZstdAdapter::is_available()) {
            auto comp = obce::ZstdAdapter::compress(sample, sizeof(sample));
            OBCE_ASSERT_TRUE(!comp.ok());
            OBCE_ASSERT_EQ(comp.status.code, obce::ErrorCode::UnsupportedFeature);
        } else {
            auto comp = obce::ZstdAdapter::compress(sample, sizeof(sample));
            OBCE_ASSERT_TRUE(comp.ok());
            auto decomp = obce::ZstdAdapter::decompress(comp.value->data(), comp.value->size());
            OBCE_ASSERT_TRUE(decomp.ok());
            OBCE_ASSERT_EQ(decomp.value->size(), sizeof(sample));
        }
    });

    obce_test::register_test("encoder_reuse_after_reset_is_idempotent", []() {
        obce::GeneratorConfig gcfg{};
        gcfg.seed = 909;
        gcfg.event_count = 80;
        const auto events = obce::generate_synthetic_events(gcfg);

        obce::CodecConfig cfg{};
        cfg.checkpoint_interval = 32;
        cfg.delta_frame_events = 16;

        obce::DeltaEncoder enc(cfg);
        OBCE_ASSERT_TRUE(enc.reset().ok());
        for (const auto& ev : events) OBCE_ASSERT_TRUE(enc.push(ev).ok());
        auto run1 = enc.finish();
        OBCE_ASSERT_TRUE(run1.ok());

        // Pushing after finish without reset must fail explicitly
        OBCE_ASSERT_EQ(enc.push(events[0]).code, obce::ErrorCode::InvalidArgument);

        // Reset and rerun -> byte-identical output
        OBCE_ASSERT_TRUE(enc.reset().ok());
        for (const auto& ev : events) OBCE_ASSERT_TRUE(enc.push(ev).ok());
        auto run2 = enc.finish();
        OBCE_ASSERT_TRUE(run2.ok());
        OBCE_ASSERT_EQ(run1.value->bytes, run2.value->bytes);
    });

    return obce_test::run_all_tests("test_operational");
}

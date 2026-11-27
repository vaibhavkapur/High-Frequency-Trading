#include "test_framework.hpp"

#include "kbnl/config.hpp"
#include "kbnl/protocol.hpp"

#include <array>
#include <string>
#include <vector>

namespace {

std::string fixture_path(const std::string& rel) {
    return std::string(KBNL_SOURCE_DIR) + "/" + rel;
}

KBNL_TEST(protocol_roundtrip_min_and_max_boundaries) {
    for (size_t pkt_size : {kbnl::kMinPacketBytes, size_t{128}, size_t{512}, kbnl::kMaxPacketBytes}) {
        std::vector<uint8_t> buf(pkt_size);
        const bool ok = kbnl::encode_deterministic_packet(
            42, 123456789ULL, 7, kbnl::kFlagBurst, 9999ULL, pkt_size, buf);
        KBNL_EXPECT_TRUE(ok);

        kbnl::DecodedPacket decoded{};
        const auto err = kbnl::decode_packet(buf, decoded);
        KBNL_EXPECT_EQ(static_cast<int>(err), static_cast<int>(kbnl::DecodeError::kOk));
        KBNL_EXPECT_EQ(decoded.header.magic, kbnl::kPacketMagic);
        KBNL_EXPECT_EQ(decoded.header.version, kbnl::kProtocolVersion);
        KBNL_EXPECT_EQ(decoded.header.flags, kbnl::kFlagBurst);
        KBNL_EXPECT_EQ(decoded.header.feed_id, 7u);
        KBNL_EXPECT_EQ(decoded.header.seq_num, 42ULL);
        KBNL_EXPECT_EQ(decoded.header.send_ts_ns, 123456789ULL);
        KBNL_EXPECT_EQ(decoded.payload.size(), pkt_size - kbnl::kPacketHeaderBytes);
    }
}

KBNL_TEST(protocol_rejects_corruption_and_invalid_frames) {
    std::array<uint8_t, 64> buf{};
    KBNL_EXPECT_TRUE(kbnl::encode_deterministic_packet(1, 1000, 1, kbnl::kFlagNone, 123, 64, buf));

    // Truncated packet
    kbnl::DecodedPacket dec{};
    KBNL_EXPECT_EQ(static_cast<int>(kbnl::decode_packet(std::span<const uint8_t>(buf.data(), 32), dec)),
                   static_cast<int>(kbnl::DecodeError::kTruncated));

    // Bad checksum (flip payload bit)
    auto bad_crc = buf;
    bad_crc[60] ^= 0x01u;
    KBNL_EXPECT_EQ(static_cast<int>(kbnl::decode_packet(bad_crc, dec)),
                   static_cast<int>(kbnl::DecodeError::kBadChecksum));

    // Bad magic
    auto bad_magic = buf;
    bad_magic[0] = 0x00u;
    KBNL_EXPECT_EQ(static_cast<int>(kbnl::decode_packet(bad_magic, dec)),
                   static_cast<int>(kbnl::DecodeError::kBadMagic));

    // Unsupported version
    auto bad_ver = buf;
    bad_ver[5] = 99u;
    KBNL_EXPECT_EQ(static_cast<int>(kbnl::decode_packet(bad_ver, dec)),
                   static_cast<int>(kbnl::DecodeError::kUnsupportedVersion));

    // Below min and above max encode rejected
    std::vector<uint8_t> small_buf(32);
    KBNL_EXPECT_FALSE(kbnl::encode_deterministic_packet(1, 1000, 1, kbnl::kFlagNone, 123, 32, small_buf));
    std::vector<uint8_t> big_buf(1500);
    KBNL_EXPECT_FALSE(kbnl::encode_deterministic_packet(1, 1000, 1, kbnl::kFlagNone, 123, 1500, big_buf));

    // Sequence 0 rejected for non-EOS packets, accepted for EOS
    KBNL_EXPECT_FALSE(kbnl::encode_deterministic_packet(0, 1000, 1, kbnl::kFlagNone, 123, 64, buf));
    KBNL_EXPECT_TRUE(kbnl::encode_deterministic_packet(0, 1000, 1, kbnl::kFlagEndOfStream, 123, 64, buf));
}

KBNL_TEST(protocol_deterministic_payload_reproducibility) {
    std::array<uint8_t, 128> p1{};
    std::array<uint8_t, 128> p2{};
    std::array<uint8_t, 128> p3{};
    KBNL_EXPECT_TRUE(kbnl::encode_deterministic_packet(77, 5000, 1, kbnl::kFlagNone, 20261127ULL, 128, p1));
    KBNL_EXPECT_TRUE(kbnl::encode_deterministic_packet(77, 5000, 1, kbnl::kFlagNone, 20261127ULL, 128, p2));
    KBNL_EXPECT_TRUE(kbnl::encode_deterministic_packet(78, 5000, 1, kbnl::kFlagNone, 20261127ULL, 128, p3));
    KBNL_EXPECT_TRUE(p1 == p2);
    KBNL_EXPECT_FALSE(p1 == p3);
}

KBNL_TEST(valid_config_fixtures_pass_validation) {
    for (const auto& f : {"fixtures/valid_baseline_config.json", "fixtures/valid_burst_config.json"}) {
        kbnl::BenchmarkConfig cfg{};
        std::vector<kbnl::ValidationError> errs;
        const bool ok = kbnl::load_config_from_json_file(fixture_path(f), cfg, errs);
        KBNL_EXPECT_TRUE(ok);
        KBNL_EXPECT_EQ(errs.size(), size_t{0});
        KBNL_EXPECT_EQ(kbnl::compute_config_hash(cfg).size(), size_t{16});
    }
}

KBNL_TEST(invalid_config_fixtures_fail_with_explicit_errors) {
    const std::vector<std::string> invalid_files = {
        "fixtures/invalid_configs/zero_rate.json",
        "fixtures/invalid_configs/bad_payload_size.json",
        "fixtures/invalid_configs/unsupported_backend.json",
        "fixtures/invalid_configs/invalid_cpu_pin.json",
        "fixtures/invalid_configs/does_not_exist.json"
    };
    for (const auto& f : invalid_files) {
        kbnl::BenchmarkConfig cfg{};
        std::vector<kbnl::ValidationError> errs;
        const bool ok = kbnl::load_config_from_json_file(fixture_path(f), cfg, errs);
        KBNL_EXPECT_FALSE(ok);
        KBNL_EXPECT_TRUE(!errs.empty());
    }
}

} // namespace

int main() {
    return kbnl::test::run_all_tests("protocol_and_config");
}

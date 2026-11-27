#include <climits>
#include <cstdint>
#include <cstring>
#include <vector>

#include "obce/crc32.hpp"
#include "obce/varint.hpp"
#include "test_harness.hpp"

int main() {
    obce_test::register_test("crc32c_standard_check_vector", []() {
        const char* sample = "123456789";
        const uint32_t c = obce::crc32c(reinterpret_cast<const uint8_t*>(sample), 9);
        // Official Castagnoli CRC32C check value for "123456789" is 0xE3069283
        OBCE_ASSERT_EQ(c, 0xE3069283U);

        // Incremental update must match single-pass
        uint32_t inc = obce::crc32c_update(0U, reinterpret_cast<const uint8_t*>(sample), 4);
        inc = obce::crc32c_update(inc, reinterpret_cast<const uint8_t*>(sample + 4), 5);
        OBCE_ASSERT_EQ(inc, 0xE3069283U);
    });

    obce_test::register_test("zigzag_boundaries_and_roundtrip", []() {
        const std::vector<int64_t> values = {
            0, -1, 1, -2, 2, -64, 63, -65, 64,
            INT32_MIN, INT32_MAX, INT64_MIN, INT64_MIN + 1, INT64_MAX - 1, INT64_MAX
        };
        OBCE_ASSERT_EQ(obce::zigzag_encode(0), 0ULL);
        OBCE_ASSERT_EQ(obce::zigzag_encode(-1), 1ULL);
        OBCE_ASSERT_EQ(obce::zigzag_encode(1), 2ULL);
        OBCE_ASSERT_EQ(obce::zigzag_encode(-2), 3ULL);
        OBCE_ASSERT_EQ(obce::zigzag_encode(INT64_MAX), UINT64_MAX - 1ULL);
        OBCE_ASSERT_EQ(obce::zigzag_encode(INT64_MIN), UINT64_MAX);

        for (int64_t v : values) {
            OBCE_ASSERT_EQ(obce::zigzag_decode(obce::zigzag_encode(v)), v);
        }
    });

    obce_test::register_test("varint_u64_and_i64_roundtrip", []() {
        const std::vector<uint64_t> uvals = {
            0ULL, 1ULL, 127ULL, 128ULL, 16383ULL, 16384ULL,
            (1ULL << 28U) - 1ULL, (1ULL << 28U),
            (1ULL << 56U) - 1ULL, (1ULL << 63U), UINT64_MAX - 1ULL, UINT64_MAX
        };
        for (uint64_t v : uvals) {
            std::vector<uint8_t> buf;
            obce::append_varint_u64(buf, v);
            OBCE_ASSERT_TRUE(buf.size() >= 1 && buf.size() <= obce::kMaxVarint64Bytes);
            size_t off = 0;
            uint64_t out = 0;
            OBCE_ASSERT_EQ(obce::decode_varint_u64(buf.data(), buf.size(), off, out), obce::ErrorCode::Ok);
            OBCE_ASSERT_EQ(out, v);
            OBCE_ASSERT_EQ(off, buf.size());
        }

        const std::vector<int64_t> ivals = {
            0, 1, -1, 1000, -1000, INT64_MIN, INT64_MAX, -1000000000000LL, 1000000000000LL
        };
        for (int64_t v : ivals) {
            std::vector<uint8_t> buf;
            obce::append_varint_i64(buf, v);
            size_t off = 0;
            int64_t out = 0;
            OBCE_ASSERT_EQ(obce::decode_varint_i64(buf.data(), buf.size(), off, out), obce::ErrorCode::Ok);
            OBCE_ASSERT_EQ(out, v);
            OBCE_ASSERT_EQ(off, buf.size());
        }
    });

    obce_test::register_test("varint_rejects_overflow_and_truncation", []() {
        // 10th byte with value 0x02 overflows uint64_t (> 64 bits)
        const uint8_t overflow_10th[10] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02};
        size_t off = 0;
        uint64_t val = 0;
        OBCE_ASSERT_EQ(obce::decode_varint_u64(overflow_10th, 10, off, val), obce::ErrorCode::VarintOverflow);

        // 11 continuation bytes
        const uint8_t overflow_11[11] = {0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x00};
        off = 0;
        OBCE_ASSERT_EQ(obce::decode_varint_u64(overflow_11, 11, off, val), obce::ErrorCode::VarintOverflow);

        // Truncated varint (MSB set on final byte)
        const uint8_t truncated[3] = {0x80, 0x80, 0x81};
        off = 0;
        OBCE_ASSERT_EQ(obce::decode_varint_u64(truncated, 3, off, val), obce::ErrorCode::TruncatedInput);

        // Empty buffer
        off = 0;
        OBCE_ASSERT_EQ(obce::decode_varint_u64(truncated, 0, off, val), obce::ErrorCode::TruncatedInput);
    });

    obce_test::register_test("wrap_safe_signed_delta_extremes", []() {
        // Transition from INT64_MIN to INT64_MAX and back without signed UB
        const uint64_t d1 = obce::signed_delta_wrap(INT64_MAX, INT64_MIN);
        OBCE_ASSERT_EQ(obce::signed_apply_delta_wrap(INT64_MIN, d1), INT64_MAX);

        const uint64_t d2 = obce::signed_delta_wrap(INT64_MIN, INT64_MAX);
        OBCE_ASSERT_EQ(obce::signed_apply_delta_wrap(INT64_MAX, d2), INT64_MIN);
    });

    return obce_test::run_all_tests("test_varint_crc");
}

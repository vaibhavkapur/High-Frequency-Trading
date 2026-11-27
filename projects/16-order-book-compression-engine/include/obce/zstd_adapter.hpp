#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "obce/types.hpp"

namespace obce {

// Optional zstd secondary compression adapter (isolated outside the core hot path).
// When built without libzstd (OBCE_HAVE_ZSTD == 0), is_available() returns false
// and compress/decompress return ErrorCode::UnsupportedFeature explicitly.
class ZstdAdapter {
public:
    static bool is_available() noexcept;
    static const char* version_string() noexcept;

    static Result<std::vector<uint8_t>> compress(
        const uint8_t* data,
        size_t len,
        int compression_level = 3);

    static Result<std::vector<uint8_t>> decompress(
        const uint8_t* data,
        size_t len,
        size_t max_output_bytes = 256U * 1024U * 1024U);
};

}  // namespace obce

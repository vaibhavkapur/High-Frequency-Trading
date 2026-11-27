#include "obce/zstd_adapter.hpp"

#if OBCE_HAVE_ZSTD
#include <zstd.h>
#endif

namespace obce {

bool ZstdAdapter::is_available() noexcept {
#if OBCE_HAVE_ZSTD
    return true;
#else
    return false;
#endif
}

const char* ZstdAdapter::version_string() noexcept {
#if OBCE_HAVE_ZSTD
    return ZSTD_versionString();
#else
    return "unavailable (built without libzstd)";
#endif
}

Result<std::vector<uint8_t>> ZstdAdapter::compress(
    const uint8_t* data,
    size_t len,
    int compression_level) {
#if OBCE_HAVE_ZSTD
    if (data == nullptr && len > 0) {
        return Result<std::vector<uint8_t>>::Err(ErrorCode::InvalidArgument, "Null input buffer");
    }
    const size_t bound = ZSTD_compressBound(len);
    std::vector<uint8_t> out(bound);
    const size_t res = ZSTD_compress(out.data(), out.size(), data, len, compression_level);
    if (ZSTD_isError(res)) {
        return Result<std::vector<uint8_t>>::Err(ErrorCode::CorruptFrame, ZSTD_getErrorName(res));
    }
    out.resize(res);
    return Result<std::vector<uint8_t>>::Ok(std::move(out));
#else
    (void)data;
    (void)len;
    (void)compression_level;
    return Result<std::vector<uint8_t>>::Err(
        ErrorCode::UnsupportedFeature,
        "Optional zstd adapter not compiled in (libzstd was not found at CMake configure time)");
#endif
}

Result<std::vector<uint8_t>> ZstdAdapter::decompress(
    const uint8_t* data,
    size_t len,
    size_t max_output_bytes) {
#if OBCE_HAVE_ZSTD
    if (data == nullptr || len == 0) {
        return Result<std::vector<uint8_t>>::Err(ErrorCode::InvalidArgument, "Empty zstd input");
    }
    const unsigned long long content_size = ZSTD_getFrameContentSize(data, len);
    if (content_size == ZSTD_CONTENTSIZE_ERROR || content_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        return Result<std::vector<uint8_t>>::Err(ErrorCode::CorruptFrame, "Invalid or unknown zstd frame size");
    }
    if (content_size > max_output_bytes) {
        return Result<std::vector<uint8_t>>::Err(ErrorCode::FrameLimitExceeded, "Decompressed size exceeds limit");
    }
    std::vector<uint8_t> out(static_cast<size_t>(content_size));
    const size_t res = ZSTD_decompress(out.data(), out.size(), data, len);
    if (ZSTD_isError(res)) {
        return Result<std::vector<uint8_t>>::Err(ErrorCode::CorruptFrame, ZSTD_getErrorName(res));
    }
    out.resize(res);
    return Result<std::vector<uint8_t>>::Ok(std::move(out));
#else
    (void)data;
    (void)len;
    (void)max_output_bytes;
    return Result<std::vector<uint8_t>>::Err(
        ErrorCode::UnsupportedFeature,
        "Optional zstd adapter not compiled in (libzstd was not found at CMake configure time)");
#endif
}

}  // namespace obce

#include "sor/quote_normalizer.hpp"

#include <cmath>
#include <cstring>

namespace sor {

namespace {

template <typename T>
void write_le(std::uint8_t* dst, T value) noexcept {
    std::memcpy(dst, &value, sizeof(T));
}

template <typename T>
T read_le(const std::uint8_t* src) noexcept {
    T value{};
    std::memcpy(&value, src, sizeof(T));
    return value;
}

std::uint32_t fnv1a_32(std::span<const std::uint8_t> data) noexcept {
    std::uint32_t hash = 2166136261U;
    for (std::uint8_t b : data) {
        hash ^= static_cast<std::uint32_t>(b);
        hash *= 16777619U;
    }
    return hash;
}

}  // namespace

std::vector<std::uint8_t> encode_wire_quote(const NormalizedQuoteUpdate& u) {
    const std::size_t count = u.level_count <= kMaxBookDepth ? u.level_count : kMaxBookDepth;
    const std::size_t body_len = kWireHeaderBytes + count * kWireLevelBytes;
    std::vector<std::uint8_t> buf(body_len + kWireChecksumBytes, 0);

    write_le<std::uint32_t>(buf.data() + 0, kWireQuoteMagic);
    buf[4] = kWireQuoteVersion;
    buf[5] = u.venue_id;
    buf[6] = static_cast<std::uint8_t>(u.type);
    buf[7] = static_cast<std::uint8_t>(u.side);
    write_le<std::uint32_t>(buf.data() + 8, u.symbol_id);
    write_le<std::uint64_t>(buf.data() + 12, u.seq);
    write_le<std::int64_t>(buf.data() + 20, u.exchange_ts_ns);
    write_le<std::int64_t>(buf.data() + 28, u.recv_ts_ns);
    buf[36] = static_cast<std::uint8_t>(count);

    std::size_t offset = kWireHeaderBytes;
    for (std::size_t i = 0; i < count; ++i) {
        write_le<std::int64_t>(buf.data() + offset, u.levels[i].price);
        write_le<std::int64_t>(buf.data() + offset + 8, u.levels[i].quantity);
        write_le<std::uint32_t>(buf.data() + offset + 16, u.levels[i].order_count);
        offset += kWireLevelBytes;
    }

    const std::uint32_t csum = fnv1a_32(std::span<const std::uint8_t>(buf.data(), body_len));
    write_le<std::uint32_t>(buf.data() + body_len, csum);
    return buf;
}

DecodeResult decode_wire_quote(std::span<const std::uint8_t> bytes) noexcept {
    DecodeResult res{};
    if (bytes.size() < kWireHeaderBytes + kWireChecksumBytes) {
        res.error = DecodeError::Truncated;
        return res;
    }

    const std::uint32_t magic = read_le<std::uint32_t>(bytes.data() + 0);
    if (magic != kWireQuoteMagic) {
        res.error = DecodeError::BadMagic;
        return res;
    }

    const std::uint8_t version = bytes[4];
    if (version != kWireQuoteVersion) {
        res.error = DecodeError::UnsupportedVersion;
        return res;
    }

    const VenueId venue_id = bytes[5];
    if (venue_id >= kMaxVenues) {
        res.error = DecodeError::InvalidVenue;
        return res;
    }

    const std::uint8_t raw_type = bytes[6];
    const std::uint8_t raw_side = bytes[7];
    if (raw_type > static_cast<std::uint8_t>(QuoteUpdateType::ClearBook) ||
        raw_side > static_cast<std::uint8_t>(Side::Sell)) {
        res.error = DecodeError::InvalidEnum;
        return res;
    }

    const std::uint8_t level_count = bytes[36];
    if (level_count > kMaxBookDepth) {
        res.error = DecodeError::TooManyLevels;
        return res;
    }

    const std::size_t expected_body =
        kWireHeaderBytes + static_cast<std::size_t>(level_count) * kWireLevelBytes;
    const std::size_t expected_total = expected_body + kWireChecksumBytes;
    if (bytes.size() != expected_total) {
        res.error = DecodeError::LengthMismatch;
        return res;
    }

    const std::uint32_t expected_csum = read_le<std::uint32_t>(bytes.data() + expected_body);
    const std::uint32_t actual_csum = fnv1a_32(bytes.subspan(0, expected_body));
    if (actual_csum != expected_csum) {
        res.error = DecodeError::ChecksumMismatch;
        return res;
    }

    NormalizedQuoteUpdate u{};
    u.venue_id = venue_id;
    u.type = static_cast<QuoteUpdateType>(raw_type);
    u.side = static_cast<Side>(raw_side);
    u.symbol_id = read_le<std::uint32_t>(bytes.data() + 8);
    u.seq = read_le<std::uint64_t>(bytes.data() + 12);
    u.exchange_ts_ns = read_le<std::int64_t>(bytes.data() + 20);
    u.recv_ts_ns = read_le<std::int64_t>(bytes.data() + 28);
    u.level_count = level_count;

    if (u.exchange_ts_ns < 0 || u.recv_ts_ns < 0) {
        res.error = DecodeError::InvalidLevelData;
        return res;
    }

    std::size_t offset = kWireHeaderBytes;
    for (std::uint8_t i = 0; i < level_count; ++i) {
        const PriceTicks price = read_le<std::int64_t>(bytes.data() + offset);
        const Quantity qty = read_le<std::int64_t>(bytes.data() + offset + 8);
        const std::uint32_t orders = read_le<std::uint32_t>(bytes.data() + offset + 16);
        // Reject non-positive prices or negative quantities; zero qty is allowed for UpsertLevel
        // (delete) but not for Snapshot.
        if (price <= 0 || qty < 0 ||
            (u.type == QuoteUpdateType::Snapshot && qty == 0)) {
            res.error = DecodeError::InvalidLevelData;
            return res;
        }
        u.levels[i] = PriceLevel{price, qty, orders};
        offset += kWireLevelBytes;
    }

    res.error = DecodeError::Ok;
    res.update = u;
    return res;
}

void QuoteNormalizer::register_venue(VenueId id, double price_scale_multiplier) noexcept {
    if (id >= kMaxVenues) return;
    venues_[id].registered = true;
    venues_[id].price_multiplier = price_scale_multiplier > 0.0 ? price_scale_multiplier : 1.0;
    venues_[id].last_seq = 0;
    venues_[id].has_seq = false;
}

bool QuoteNormalizer::normalize(const NormalizedQuoteUpdate& raw,
                                NormalizedQuoteUpdate& out) noexcept {
    if (raw.venue_id >= kMaxVenues || !venues_[raw.venue_id].registered) {
        ++stats_.messages_rejected;
        return false;
    }
    if (raw.level_count > kMaxBookDepth || raw.exchange_ts_ns < 0 || raw.recv_ts_ns < 0) {
        ++stats_.messages_rejected;
        return false;
    }

    auto& vs = venues_[raw.venue_id];
    if (raw.seq > 0) {
        if (vs.has_seq) {
            if (raw.seq <= vs.last_seq) {
                ++stats_.out_of_order_dropped;
                return false;
            }
            if (raw.seq > vs.last_seq + 1) {
                ++stats_.sequence_gaps;
            }
        }
        vs.last_seq = raw.seq;
        vs.has_seq = true;
    }

    out = raw;
    if (out.recv_ts_ns < out.exchange_ts_ns) {
        // Clock skew guard: clamp recv_ts to exchange_ts so feed latency is non-negative.
        out.recv_ts_ns = out.exchange_ts_ns;
        ++stats_.negative_latency_clamped;
    }

    const double mult = vs.price_multiplier;
    for (std::uint8_t i = 0; i < out.level_count; ++i) {
        if (out.levels[i].price <= 0 || out.levels[i].quantity < 0) {
            ++stats_.messages_rejected;
            return false;
        }
        if (mult != 1.0) {
            const double scaled = static_cast<double>(out.levels[i].price) * mult;
            if (!std::isfinite(scaled) || scaled <= 0.0 ||
                scaled > static_cast<double>(std::numeric_limits<PriceTicks>::max() / 4)) {
                ++stats_.messages_rejected;
                return false;
            }
            out.levels[i].price = static_cast<PriceTicks>(std::llround(scaled));
        }
    }

    ++stats_.messages_accepted;
    return true;
}

DecodeError QuoteNormalizer::decode_and_normalize(std::span<const std::uint8_t> frame,
                                                  NormalizedQuoteUpdate& out) noexcept {
    const DecodeResult decoded = decode_wire_quote(frame);
    if (!decoded.ok()) {
        ++stats_.messages_rejected;
        return decoded.error;
    }
    if (!normalize(decoded.update, out)) {
        return DecodeError::InvalidLevelData;
    }
    return DecodeError::Ok;
}

void QuoteNormalizer::reset_stats() noexcept {
    stats_ = NormalizerStats{};
    for (auto& v : venues_) {
        v.last_seq = 0;
        v.has_seq = false;
    }
}

}  // namespace sor

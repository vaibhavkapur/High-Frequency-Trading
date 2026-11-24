// Small deterministic PRNG. std:: distributions are implementation-defined,
// so everything that must reproduce bit-for-bit across platforms goes here.
#pragma once

#include <cstdint>

namespace hftbt {

class Rng {
public:
    explicit Rng(std::uint64_t seed = 0) noexcept { reseed(seed); }

    void reseed(std::uint64_t seed) noexcept {
        for (auto& s : s_) { // splitmix64
            seed += 0x9E3779B97F4A7C15ULL;
            std::uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            s = z ^ (z >> 31);
        }
    }

    std::uint64_t next() noexcept { // xoshiro256**
        const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    // Uniform in [0, n). n == 0 returns 0. (Multiply-shift; bias is < 2^-32 for
    // the ranges used here and irrelevant next to determinism.)
    std::uint64_t below(std::uint64_t n) noexcept {
        return static_cast<std::uint64_t>((static_cast<unsigned __int128>(next()) * n) >> 64);
    }

    // Uniform in [lo, hi] inclusive.
    std::int64_t range(std::int64_t lo, std::int64_t hi) noexcept {
        return lo + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(hi - lo) + 1));
    }

    bool chance(std::uint32_t percent) noexcept { return below(100) < percent; }

private:
    static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept {
        return (x << k) | (x >> (64 - k));
    }
    std::uint64_t s_[4]{};
};

} // namespace hftbt

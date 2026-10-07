#include "crowdbook/random.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "crowdbook/math.hpp"

namespace crowdbook {

namespace {

constexpr std::uint64_t kGoldenGamma = 0x9e3779b97f4a7c15;

// SplitMix64's output function, a bijective mix of 64 bits.
constexpr std::uint64_t mix(std::uint64_t z) noexcept {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
}

} // namespace

Random::Random(std::uint64_t seed, std::uint64_t stream) noexcept {
    // Fold seed and stream into one starting point, then expand it into xoshiro's 256-bit state
    // with SplitMix64, as the xoshiro authors recommend.
    std::uint64_t splitMix = mix(seed ^ mix(stream));
    for (std::uint64_t& word : state_) {
        splitMix += kGoldenGamma;
        word = mix(splitMix);
    }
}

std::uint64_t Random::next() noexcept {
    // xoshiro256** by David Blackman and Sebastiano Vigna.
    const std::uint64_t result = std::rotl(state_[1] * 5, 7) * 9;
    const std::uint64_t shifted = state_[1] << 17;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= shifted;
    state_[3] = std::rotl(state_[3], 45);
    return result;
}

std::uint64_t Random::below(std::uint64_t bound) {
    if (bound == 0) {
        throw std::invalid_argument("bound must be positive");
    }
    // Rejecting the lowest 2^64 mod bound values makes every remainder equally likely.
    const std::uint64_t threshold = (std::uint64_t{0} - bound) % bound;
    while (true) {
        const std::uint64_t bits = next();
        if (bits >= threshold) {
            return bits % bound;
        }
    }
}

std::int64_t Random::uniformInt(std::int64_t low, std::int64_t high) {
    if (low > high) {
        throw std::invalid_argument("low must not exceed high");
    }
    const auto span = static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low);
    const std::uint64_t offset =
        span == std::numeric_limits<std::uint64_t>::max() ? next() : below(span + 1);
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(low) + offset);
}

double Random::uniform() noexcept {
    // The top 53 bits scaled into [0, 1), so every result is a multiple of 2^-53.
    return static_cast<double>(next() >> 11) * 0x1.0p-53;
}

bool Random::bernoulli(double probability) noexcept { return uniform() < probability; }

double Random::exponential(double rate) {
    if (!(rate > 0.0)) {
        throw std::invalid_argument("rate must be positive");
    }
    // Inverse transform sampling; 1 - u is in (0, 1], so the logarithm is finite.
    return -math::log(1.0 - uniform()) / rate;
}

double Random::normal(double mean, double stddev) {
    if (!(stddev >= 0.0)) {
        throw std::invalid_argument("stddev must not be negative");
    }
    // Marsaglia's polar method. The second value of each pair is dropped so the stream keeps no
    // hidden state between calls.
    while (true) {
        const double x = 2.0 * uniform() - 1.0;
        const double y = 2.0 * uniform() - 1.0;
        const double s = x * x + y * y;
        if (s > 0.0 && s < 1.0) {
            return mean + stddev * x * std::sqrt(-2.0 * math::log(s) / s);
        }
    }
}

} // namespace crowdbook

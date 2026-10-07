#pragma once

#include <array>
#include <cstdint>

namespace crowdbook {

// A random number stream named by a seed and a stream number, for example the run's seed and an
// agent's id. Different streams are independent, and the same pair always produces the same
// sequence.
//
// The engine (xoshiro256**, seeded through SplitMix64) and every distribution are implemented here
// instead of taken from <random>, whose distributions are implementation-defined, and the
// floating-point draws use crowdbook::math's logarithm and the correctly rounded square root, so
// every draw is identical on every platform.
class Random {
public:
    Random(std::uint64_t seed, std::uint64_t stream) noexcept;

    // The next 64 random bits.
    std::uint64_t next() noexcept;
    // Uniform in [0, bound). Throws std::invalid_argument if bound is 0.
    std::uint64_t below(std::uint64_t bound);
    // Uniform in [low, high], inclusive. Throws std::invalid_argument if low > high.
    std::int64_t uniformInt(std::int64_t low, std::int64_t high);
    // Uniform in [0, 1).
    double uniform() noexcept;
    // True with the given probability.
    bool bernoulli(double probability) noexcept;
    // Exponentially distributed with mean 1 / rate. Throws std::invalid_argument unless rate > 0.
    double exponential(double rate);
    // Normally distributed. Throws std::invalid_argument if stddev is negative.
    double normal(double mean, double stddev);

private:
    std::array<std::uint64_t, 4> state_{};
};

} // namespace crowdbook

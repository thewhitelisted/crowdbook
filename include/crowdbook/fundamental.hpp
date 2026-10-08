#pragma once

#include <cstdint>
#include <optional>

#include "crowdbook/random.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

struct FundamentalConfig {
    double initial = 10'000.0;           // ticks; also the level the value reverts to
    double meanReversion = 0.0;          // per second; 0 makes the value a random walk
    double volatility = 1.0;             // ticks per square root of a second
    Duration step = 100 * kMillisecond;  // how often the value changes
    // News: jumps at random times, a Poisson process with this many a second on average, each
    // normally distributed with standard deviation jumpSize ticks. 0 means no news.
    double jumpRate = 0.0;
    double jumpSize = 0.0;
};

// A prediction market's share pays this many cents if the answer is yes, and nothing if it is no,
// so it trades from 1 to one less.
inline constexpr Price kPredictionPayout = 100;

// A prediction market's question, decided at the end of the run by whether a hidden quantity ends
// above zero. The quantity wanders, and news moves it in jumps; its change over the whole run has
// variance 1. The true value is the probability of yes, in cents.
struct PredictionConfig {
    double probability = 0.5; // of yes at the start, strictly between 0 and 1
    double newsRate = 0.0;    // news a second, on average; 0 means no news
    // The share of the outcome's uncertainty that news carries, at least 0 and below 1; the rest
    // comes from the quantity's steady wandering. 0 without news.
    double newsShare = 0.0;
    Duration step = kSecond; // how often the hidden quantity moves

    friend bool operator==(const PredictionConfig&, const PredictionConfig&) = default;
};

// The asset's true value over time, which informed agents observe with noise. It follows an
// Ornstein–Uhlenbeck process, or a random walk when meanReversion is 0, updated every `step` with
// the exact discrete-time formulas, plus jumps when there is news. It draws only from its own
// random stream, so the path depends on that stream and the config, never on who reads it or
// when; without news it makes no draws for jumps, so turning news off leaves the path as it was.
//
// For a prediction market the walk is the hidden quantity and the value is the exact probability,
// given the path so far, that it ends above zero: a sum over how much news is still to come of
// Φ(x / √(the variance left)). It starts where that probability is the configured one, and at
// resolution it is 100 or 0.
class Fundamental {
public:
    // Throws std::invalid_argument for a step that is not positive or a negative volatility, mean
    // reversion, jump rate or jump size.
    Fundamental(const FundamentalConfig& config, Random random);
    // A prediction market's value, resolving at `resolution`. Throws std::invalid_argument for a
    // probability outside (0, 1), a negative news rate, a news share outside [0, 1) or one without
    // news, a step that is not positive, or a resolution that is not after the start.
    Fundamental(const PredictionConfig& config, Duration resolution, Random random);

    // The value at `time`. Reads must not go back to an earlier step than a previous read, which
    // holds inside a simulation because its clock never runs backwards; throws
    // std::invalid_argument otherwise.
    [[nodiscard]] double valueAt(Timestamp time);
    // How many jumps the value has taken so far.
    [[nodiscard]] std::int64_t jumps() const noexcept { return jumps_; }
    // When a prediction market resolves; nullopt for any other value.
    [[nodiscard]] std::optional<Duration> resolution() const noexcept { return resolution_; }
    // The walk behind the value as of the last read: the value itself, or for a prediction market
    // the hidden quantity that decides it, whose change over the whole run has variance 1. Reading
    // it moves nothing.
    [[nodiscard]] double walk() const noexcept { return value_; }

private:
    // Moves the walk on by one step, with this standard deviation and expected number of jumps.
    void move(double shockScale, double jumpsExpected);
    // The probability, in cents, that the hidden quantity ends above zero from `x` with `seconds`
    // to go.
    [[nodiscard]] double probabilityOfYes(double x, double seconds) const noexcept;

    FundamentalConfig config_;
    Random random_;
    std::int64_t step_ = 0; // index of the step the current value belongs to
    double value_ = 0.0;
    double decay_ = 1.0;      // how much of the gap to the mean survives one step
    double shockScale_ = 0.0; // standard deviation of the change in one step
    double jumpsPerStep_ = 0.0;
    std::int64_t jumps_ = 0;
    // For a prediction market: when it resolves, the hidden quantity's variance per second
    // between news, each piece of news's variance, the value at the current step and the outcome.
    std::optional<Duration> resolution_{};
    double variancePerSecond_ = 0.0;
    double newsVariance_ = 0.0;
    std::int64_t valuedStep_ = -1;
    double probability_ = 0.0;
    std::optional<double> outcome_{};
};

} // namespace crowdbook

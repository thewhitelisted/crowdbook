#pragma once

#include <cstdint>

#include "crowdbook/random.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

struct FundamentalConfig {
    double initial = 10'000.0;           // ticks; also the level the value reverts to
    double meanReversion = 0.0;          // per second; 0 makes the value a random walk
    double volatility = 1.0;             // ticks per square root of a second
    Duration step = 100 * kMillisecond;  // how often the value changes
};

// The asset's true value over time, which informed agents observe with noise. It follows an
// Ornstein–Uhlenbeck process, or a random walk when meanReversion is 0, updated every `step` with
// the exact discrete-time formulas. It draws only from its own random stream, so the path depends
// on that stream and the config, never on who reads it or when.
class Fundamental {
public:
    // Throws std::invalid_argument for a step that is not positive or a negative volatility or
    // mean reversion.
    Fundamental(const FundamentalConfig& config, Random random);

    // The value at `time`. Reads must not go back to an earlier step than a previous read, which
    // holds inside a simulation because its clock never runs backwards; throws
    // std::invalid_argument otherwise.
    [[nodiscard]] double valueAt(Timestamp time);

private:
    FundamentalConfig config_;
    Random random_;
    std::int64_t step_ = 0; // index of the step the current value belongs to
    double value_ = 0.0;
    double decay_ = 1.0;      // how much of the gap to the mean survives one step
    double shockScale_ = 0.0; // standard deviation of the change in one step
};

} // namespace crowdbook

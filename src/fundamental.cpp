#include "crowdbook/fundamental.hpp"

#include <cmath>
#include <format>
#include <stdexcept>

namespace crowdbook {

Fundamental::Fundamental(const FundamentalConfig& config, Random random)
    : config_(config), random_(random), value_(config.initial) {
    if (config.step <= 0) {
        throw std::invalid_argument("the fundamental's step must be positive");
    }
    for (const double setting : {config.initial, config.volatility, config.meanReversion,
                                 config.jumpRate, config.jumpSize}) {
        if (!std::isfinite(setting)) {
            throw std::invalid_argument("the fundamental's settings must be finite numbers");
        }
    }
    if (!(config.volatility >= 0.0) || !(config.meanReversion >= 0.0)) {
        throw std::invalid_argument(
            "the fundamental's volatility and mean reversion must not be negative");
    }
    if (!(config.jumpRate >= 0.0) || !(config.jumpSize >= 0.0)) {
        throw std::invalid_argument("the jump rate and jump size must not be negative");
    }
    const double seconds = static_cast<double>(config.step) / static_cast<double>(kSecond);
    jumpsPerStep_ = config.jumpRate * seconds;
    const double theta = config.meanReversion;
    if (theta > 0.0) {
        decay_ = std::exp(-theta * seconds);
        shockScale_ = config.volatility * std::sqrt((1.0 - decay_ * decay_) / (2.0 * theta));
    } else {
        decay_ = 1.0;
        shockScale_ = config.volatility * std::sqrt(seconds);
    }
}

double Fundamental::valueAt(Timestamp time) {
    const std::int64_t target = time < 0 ? 0 : time / config_.step;
    if (target < step_) {
        throw std::invalid_argument(std::format(
            "the fundamental was read at step {} after step {}; reads cannot go back in time",
            target, step_));
    }
    for (; step_ < target; ++step_) {
        value_ = config_.initial + (value_ - config_.initial) * decay_ +
                 shockScale_ * random_.normal(0.0, 1.0);
        if (jumpsPerStep_ > 0.0) {
            // The number of jumps in one step is Poisson: walk its distribution to a uniform draw.
            const double draw = random_.uniform();
            double probability = std::exp(-jumpsPerStep_);
            double cumulative = probability;
            std::int64_t count = 0;
            while (draw > cumulative && probability > 0.0) {
                ++count;
                probability *= jumpsPerStep_ / static_cast<double>(count);
                cumulative += probability;
            }
            for (std::int64_t jump = 0; jump < count; ++jump) {
                value_ += random_.normal(0.0, config_.jumpSize);
            }
            jumps_ += count;
        }
    }
    return value_;
}

} // namespace crowdbook

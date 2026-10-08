#include "crowdbook/fundamental.hpp"

#include <cmath>
#include <format>
#include <stdexcept>

#include "crowdbook/math.hpp"

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
        decay_ = math::exp(-theta * seconds);
        shockScale_ = config.volatility * std::sqrt((1.0 - decay_ * decay_) / (2.0 * theta));
    } else {
        decay_ = 1.0;
        shockScale_ = config.volatility * std::sqrt(seconds);
    }
}

namespace {

// What the walk of a prediction market is: no pull towards a mean, and the given variances.
FundamentalConfig walkOf(const PredictionConfig& config, Duration resolution) {
    if (!(config.probability > 0.0 && config.probability < 1.0)) {
        throw std::invalid_argument("the probability must be between 0 and 1");
    }
    if (!(config.newsRate >= 0.0) || !std::isfinite(config.newsRate)) {
        throw std::invalid_argument("the news rate must not be negative");
    }
    if (!(config.newsShare >= 0.0 && config.newsShare < 1.0)) {
        throw std::invalid_argument("the news share must be at least 0 and below 1");
    }
    if (config.newsShare > 0.0 && config.newsRate == 0.0) {
        throw std::invalid_argument("a news share needs news: give a news rate");
    }
    if (config.step <= 0 || resolution <= 0) {
        throw std::invalid_argument("the step and the time to resolution must be positive");
    }
    const double seconds = static_cast<double>(resolution) / static_cast<double>(kSecond);
    if (config.newsRate * seconds > 500.0) {
        throw std::invalid_argument(
            "at most 500 pieces of news are expected before resolution; lower the news rate");
    }
    return {.initial = 0.0,
            .meanReversion = 0.0,
            .volatility = std::sqrt((1.0 - config.newsShare) / seconds),
            .step = config.step,
            .jumpRate = config.newsRate,
            .jumpSize = config.newsRate > 0.0
                            ? std::sqrt(config.newsShare / (config.newsRate * seconds))
                            : 0.0};
}

} // namespace

Fundamental::Fundamental(const PredictionConfig& config, Duration resolution, Random random)
    : Fundamental(walkOf(config, resolution), random) {
    resolution_ = resolution;
    variancePerSecond_ = config_.volatility * config_.volatility;
    newsVariance_ = config_.jumpSize * config_.jumpSize;
    // Start where the probability of yes is the one asked for. It rises with the starting point,
    // so bisection finds it, the same way everywhere.
    const double seconds = static_cast<double>(resolution) / static_cast<double>(kSecond);
    const double target = 100.0 * config.probability;
    double low = -40.0;
    double high = 40.0;
    for (int i = 0; i < 200 && low < high; ++i) {
        const double middle = (low + high) / 2.0;
        if (middle == low || middle == high) {
            break;
        }
        (probabilityOfYes(middle, seconds) < target ? low : high) = middle;
    }
    value_ = (low + high) / 2.0;
}

void Fundamental::move(double shockScale, double jumpsExpected) {
    value_ = config_.initial + (value_ - config_.initial) * decay_ +
             shockScale * random_.normal(0.0, 1.0);
    if (jumpsExpected > 0.0) {
        // The number of jumps in one step is Poisson: walk its distribution to a uniform draw.
        const double draw = random_.uniform();
        double probability = math::exp(-jumpsExpected);
        double cumulative = probability;
        std::int64_t count = 0;
        while (draw > cumulative && probability > 0.0) {
            ++count;
            probability *= jumpsExpected / static_cast<double>(count);
            cumulative += probability;
        }
        for (std::int64_t jump = 0; jump < count; ++jump) {
            value_ += random_.normal(0.0, config_.jumpSize);
        }
        jumps_ += count;
    }
}

double Fundamental::probabilityOfYes(double x, double seconds) const noexcept {
    // The change still to come is normal with variance variancePerSecond_ * seconds, plus a
    // Poisson number of pieces of news, each normal with variance newsVariance_: sum over the
    // number of pieces, until the rest of the Poisson weights is negligible.
    const double steady = variancePerSecond_ * seconds;
    const double expected = config_.jumpRate * seconds;
    double weight = math::exp(-expected);
    double sum = 0.0;
    for (int pieces = 0;; ++pieces) {
        if (pieces > 0) {
            weight *= expected / static_cast<double>(pieces);
        }
        // Terms this unlikely cannot change a probability in cents, so they are not worked out.
        if (weight > 1e-20) {
            const double variance = steady + static_cast<double>(pieces) * newsVariance_;
            const double cdf = variance > 0.0 ? math::normalCdf(x / std::sqrt(variance))
                                              : (x > 0.0 ? 1.0 : 0.0);
            sum += weight * cdf;
        }
        // Past the most likely number of pieces the weights only shrink.
        if (static_cast<double>(pieces) >= expected && weight < 1e-18) {
            break;
        }
    }
    return 100.0 * sum;
}

double Fundamental::valueAt(Timestamp time) {
    if (resolution_ && time >= *resolution_) {
        if (!outcome_) {
            // Every whole step up to resolution, then whatever is left of the last one.
            const std::int64_t last = *resolution_ / config_.step;
            if (last < step_) {
                throw std::invalid_argument("the fundamental cannot go back in time");
            }
            for (; step_ < last; ++step_) {
                move(shockScale_, jumpsPerStep_);
            }
            const Duration rest = *resolution_ - last * config_.step;
            if (rest > 0) {
                const double seconds = static_cast<double>(rest) / static_cast<double>(kSecond);
                move(config_.volatility * std::sqrt(seconds), config_.jumpRate * seconds);
            }
            outcome_ = value_ > 0.0 ? 100.0 : 0.0;
        }
        return *outcome_;
    }
    const std::int64_t target = time < 0 ? 0 : time / config_.step;
    if (target < step_) {
        throw std::invalid_argument(std::format(
            "the fundamental was read at step {} after step {}; reads cannot go back in time",
            target, step_));
    }
    for (; step_ < target; ++step_) {
        move(shockScale_, jumpsPerStep_);
    }
    if (!resolution_) {
        return value_;
    }
    // The probability changes only when the walk moves: work it out once a step.
    if (valuedStep_ != step_) {
        const Duration left = *resolution_ - step_ * config_.step;
        probability_ = probabilityOfYes(value_, static_cast<double>(left) /
                                                    static_cast<double>(kSecond));
        valuedStep_ = step_;
    }
    return probability_;
}

} // namespace crowdbook

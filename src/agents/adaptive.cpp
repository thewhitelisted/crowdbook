#include "crowdbook/agents/adaptive.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace crowdbook {

namespace {

// The weight of a new sample in an exponential average sampled every `interval` with the given
// half-life, or the decay of a sum over one interval when used as 1 - weight.
double weightFor(Duration interval, Duration halfLife) {
    return 1.0 - std::exp2(-static_cast<double>(interval) / static_cast<double>(halfLife));
}

} // namespace

AdaptiveTrader::AdaptiveTrader(const AdaptiveConfig& config,
                               std::shared_ptr<Fundamental> fundamental, Price referencePrice)
    : config_(config), fundamental_(std::move(fundamental)), market_(referencePrice) {
    if (!fundamental_) {
        throw std::invalid_argument(
            "adaptive traders need a fundamental value; add a [fundamental] section");
    }
    if (config.interval <= 0 || config.memory <= 0 || !(config.noise >= 0.0) ||
        !(config.choiceIntensity >= 0.0) || !(config.threshold >= 0.0)) {
        throw std::invalid_argument("adaptive needs a positive interval and memory, and noise, "
                                    "choice_intensity and threshold that are not negative");
    }
    if (config.fastHalfLife <= 0 || config.slowHalfLife <= config.fastHalfLife) {
        throw std::invalid_argument("adaptive needs 0 < fast_half_life < slow_half_life");
    }
    if (config.orderSize < 1 || config.maxPosition < config.orderSize) {
        throw std::invalid_argument("adaptive needs 1 <= order_size <= max_position");
    }
    fastWeight_ = weightFor(config.interval, config.fastHalfLife);
    slowWeight_ = weightFor(config.interval, config.slowHalfLife);
    recordDecay_ = 1.0 - weightFor(config.interval, config.memory);
}

void AdaptiveTrader::onStart(AgentContext& context) { context.wakeWithin(config_.interval); }

int AdaptiveTrader::call(double signal) const noexcept {
    if (signal >= config_.threshold) {
        return 1;
    }
    return signal <= -config_.threshold ? -1 : 0;
}

void AdaptiveTrader::onWakeup(AgentContext& context, std::uint64_t /*tag*/) {
    market_.update(context.market());
    const double price = market_.fairPrice();
    Random& random = context.random();
    const double noise = random.normal(0.0, config_.noise);
    const double value = fundamental_->valueAt(context.now()) + noise;

    // Score the calls made last time against what the price has done since.
    if (lastPrice_) {
        const double change = price - *lastPrice_;
        valueRecord_ = valueRecord_ * recordDecay_ + lastValueCall_ * change;
        trendRecord_ = trendRecord_ * recordDecay_ + lastTrendCall_ * change;
    }
    if (fast_ && slow_) {
        *fast_ += fastWeight_ * (price - *fast_);
        *slow_ += slowWeight_ * (price - *slow_);
    } else {
        fast_ = price;
        slow_ = price;
    }
    const int valueCall = call(value - price);
    const int trendCall = call(*fast_ - *slow_);

    const double odds = config_.choiceIntensity * (trendRecord_ - valueRecord_);
    const double draw = random.uniform();
    followingTrend_ = draw < 1.0 / (1.0 + std::exp(-odds));
    const int direction = followingTrend_ ? trendCall : valueCall;

    // Trade toward the position the chosen strategy wants, counting orders in flight as filled.
    const Ledger& ledger = context.ledger();
    const Quantity exposure =
        ledger.position() + ledger.openQuantity(Side::Buy) - ledger.openQuantity(Side::Sell);
    const Quantity target = direction * config_.maxPosition;
    if (target > exposure) {
        context.submitMarket(Side::Buy, std::min(config_.orderSize, target - exposure));
    } else if (target < exposure) {
        context.submitMarket(Side::Sell, std::min(config_.orderSize, exposure - target));
    }

    lastPrice_ = price;
    lastValueCall_ = valueCall;
    lastTrendCall_ = trendCall;
    context.wakeAfter(config_.interval);
}

} // namespace crowdbook

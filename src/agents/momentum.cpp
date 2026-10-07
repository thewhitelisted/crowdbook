#include "crowdbook/agents/momentum.hpp"

#include <cmath>
#include <stdexcept>

namespace crowdbook {

namespace {

// The weight of each new sample in a moving average that halves an old value's influence every
// `halfLife`, when sampled every `interval`.
double weightFor(Duration interval, Duration halfLife) {
    return 1.0 - std::exp2(-static_cast<double>(interval) / static_cast<double>(halfLife));
}

} // namespace

MomentumTrader::MomentumTrader(const MomentumConfig& config, Price referencePrice)
    : config_(config), market_(referencePrice) {
    if (config.interval <= 0 || config.fastHalfLife <= 0 ||
        config.fastHalfLife >= config.slowHalfLife) {
        throw std::invalid_argument("momentum needs a positive interval and "
                                    "0 < fast_half_life < slow_half_life");
    }
    if (!(config.threshold >= 0.0) || config.orderSize < 1 ||
        config.maxPosition < config.orderSize) {
        throw std::invalid_argument(
            "momentum needs threshold >= 0 and 1 <= order_size <= max_position");
    }
    fastWeight_ = weightFor(config.interval, config.fastHalfLife);
    slowWeight_ = weightFor(config.interval, config.slowHalfLife);
}

void MomentumTrader::onStart(AgentContext& context) { context.wakeWithin(config_.interval); }

void MomentumTrader::onWakeup(AgentContext& context, std::uint64_t /*tag*/) {
    market_.update(context.market());
    const double price = market_.fairPrice();
    if (fast_ && slow_) {
        *fast_ += fastWeight_ * (price - *fast_);
        *slow_ += slowWeight_ * (price - *slow_);
    } else {
        fast_ = price;
        slow_ = price;
    }

    const Ledger& ledger = context.ledger();
    const Quantity size = config_.orderSize;
    if (trend() > config_.threshold &&
        ledger.position() + ledger.openQuantity(Side::Buy) + size <= config_.maxPosition) {
        context.submitMarket(Side::Buy, size);
    } else if (trend() < -config_.threshold &&
               ledger.position() - ledger.openQuantity(Side::Sell) - size >=
                   -config_.maxPosition) {
        context.submitMarket(Side::Sell, size);
    }
    context.wakeAfter(config_.interval);
}

double MomentumTrader::trend() const noexcept {
    return fast_ && slow_ ? *fast_ - *slow_ : 0.0;
}

} // namespace crowdbook

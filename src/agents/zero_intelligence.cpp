#include "crowdbook/agents/zero_intelligence.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace crowdbook {

namespace {

constexpr std::uint64_t kNextOrder = 0; // wakeup tag; client order ids start at 1

Side randomSide(Random& random) { return random.below(2) == 0 ? Side::Buy : Side::Sell; }

Duration secondsToDuration(double seconds) {
    return std::max<Duration>(static_cast<Duration>(seconds * static_cast<double>(kSecond)), 1);
}

} // namespace

ZeroIntelligenceTrader::ZeroIntelligenceTrader(const ZeroIntelligenceConfig& config,
                                               Price referencePrice)
    : config_(config), market_(referencePrice) {
    if (!(config.limitRate >= 0.0) || !(config.marketRate >= 0.0) ||
        !(config.cancelRate >= 0.0) || !(orderRate() > 0.0)) {
        throw std::invalid_argument("zero_intelligence rates must not be negative, and limit_rate "
                                    "or market_rate must be positive");
    }
    if (config.maxOffset < 1) {
        throw std::invalid_argument("zero_intelligence max_offset must be at least 1");
    }
    if (config.minSize < 1 || config.minSize > config.maxSize) {
        throw std::invalid_argument("zero_intelligence sizes need 1 <= min_size <= max_size");
    }
}

void ZeroIntelligenceTrader::onStart(AgentContext& context) { scheduleNextOrder(context); }

void ZeroIntelligenceTrader::onWakeup(AgentContext& context, std::uint64_t tag) {
    if (tag != kNextOrder) {
        // A resting order's lifetime is over, unless it filled or is already being cancelled.
        const OwnOrder* order = context.ledger().find(tag);
        if (order != nullptr && !order->cancelRequested) {
            context.cancel(tag);
        }
        return;
    }

    market_.update(context.market());
    // Picking limit or market in proportion to its rate, with one exponential timer for both, is
    // the same as running two independent Poisson processes.
    if (context.random().uniform() * orderRate() < config_.limitRate) {
        sendLimit(context);
    } else {
        sendMarket(context);
    }
    scheduleNextOrder(context);
}

double ZeroIntelligenceTrader::orderRate() const noexcept {
    return config_.limitRate + config_.marketRate;
}

void ZeroIntelligenceTrader::scheduleNextOrder(AgentContext& context) const {
    context.wakeAt(context.now() + secondsToDuration(context.random().exponential(orderRate())),
                   kNextOrder);
}

void ZeroIntelligenceTrader::sendLimit(AgentContext& context) const {
    Random& random = context.random();
    const Side side = randomSide(random);
    const Price offset = random.uniformInt(1, config_.maxOffset);
    const Quantity size = random.uniformInt(config_.minSize, config_.maxSize);

    const auto fair = static_cast<Price>(std::llround(market_.fairPrice()));
    const Price anchor = side == Side::Buy ? market_.bestAsk().value_or(fair + 1)
                                           : market_.bestBid().value_or(fair - 1);
    const Price price = side == Side::Buy ? anchor - offset : anchor + offset;
    const ClientOrderId id = context.submitLimit(side, std::max<Price>(price, 1), size);
    if (config_.cancelRate > 0.0) {
        const double lifetime = random.exponential(config_.cancelRate);
        context.wakeAt(context.now() + secondsToDuration(lifetime), id);
    }
}

void ZeroIntelligenceTrader::sendMarket(AgentContext& context) const {
    Random& random = context.random();
    const Side side = randomSide(random);
    const Quantity size = random.uniformInt(config_.minSize, config_.maxSize);
    context.submitMarket(side, size);
}

} // namespace crowdbook

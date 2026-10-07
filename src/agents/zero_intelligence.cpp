#include "crowdbook/agents/zero_intelligence.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace crowdbook {

namespace {

Side randomSide(Random& random) { return random.below(2) == 0 ? Side::Buy : Side::Sell; }

} // namespace

ZeroIntelligenceTrader::ZeroIntelligenceTrader(const ZeroIntelligenceConfig& config,
                                               Price referencePrice)
    : config_(config), market_(referencePrice) {
    if (!(config.limitRate >= 0.0) || !(config.marketRate >= 0.0) ||
        !(config.cancelRate >= 0.0) || !(totalRate() > 0.0)) {
        throw std::invalid_argument(
            "zero_intelligence rates must not be negative, and at least one must be positive");
    }
    if (config.maxOffset < 1) {
        throw std::invalid_argument("zero_intelligence max_offset must be at least 1");
    }
    if (config.minSize < 1 || config.minSize > config.maxSize) {
        throw std::invalid_argument("zero_intelligence sizes need 1 <= min_size <= max_size");
    }
}

void ZeroIntelligenceTrader::onStart(AgentContext& context) { scheduleNext(context); }

void ZeroIntelligenceTrader::onWakeup(AgentContext& context, std::uint64_t /*tag*/) {
    market_.update(context.market());
    // Picking an action in proportion to its rate, with one exponential timer for all of them, is
    // the same as running three independent Poisson processes.
    const double pick = context.random().uniform() * totalRate();
    if (pick < config_.limitRate) {
        sendLimit(context);
    } else if (pick < config_.limitRate + config_.marketRate) {
        sendMarket(context);
    } else {
        cancelOne(context);
    }
    scheduleNext(context);
}

double ZeroIntelligenceTrader::totalRate() const noexcept {
    return config_.limitRate + config_.marketRate + config_.cancelRate;
}

void ZeroIntelligenceTrader::scheduleNext(AgentContext& context) const {
    const double seconds = context.random().exponential(totalRate());
    const auto delay = static_cast<Duration>(seconds * static_cast<double>(kSecond));
    context.wakeAfter(std::max<Duration>(delay, 1));
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
    context.submitLimit(side, std::max<Price>(price, 1), size);
}

void ZeroIntelligenceTrader::sendMarket(AgentContext& context) const {
    Random& random = context.random();
    const Side side = randomSide(random);
    const Quantity size = random.uniformInt(config_.minSize, config_.maxSize);
    context.submitMarket(side, size);
}

void ZeroIntelligenceTrader::cancelOne(AgentContext& context) {
    std::vector<ClientOrderId> candidates;
    for (const auto& [clientOrderId, order] : context.ledger().orders()) {
        if (order.type == OrderType::Limit && !order.cancelRequested) {
            candidates.push_back(clientOrderId);
        }
    }
    if (candidates.empty()) {
        return;
    }
    const auto index = static_cast<std::size_t>(context.random().below(candidates.size()));
    context.cancel(candidates[index]);
}

} // namespace crowdbook

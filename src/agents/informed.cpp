#include "crowdbook/agents/informed.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include <utility>

#include "crowdbook/agents/execution.hpp"

namespace crowdbook {

InformedTrader::InformedTrader(const InformedConfig& config,
                               std::shared_ptr<Fundamental> fundamental)
    : config_(config), belief_(config.belief, std::move(fundamental)) {
    if (config.interval <= 0 || !(config.threshold >= 0.0)) {
        throw std::invalid_argument(
            "informed needs a positive interval and a threshold that is not negative");
    }
    if (config.orderSize < 1 || config.maxPosition < config.orderSize ||
        config.maxPosition > kMaxQuantity) {
        throw std::invalid_argument(std::format(
            "informed needs 1 <= order_size <= max_position <= {}", kMaxQuantity));
    }
    if (!(config.sizeTail >= 0.0) || !std::isfinite(config.sizeTail)) {
        throw std::invalid_argument("informed size_tail must not be negative");
    }
}

Quantity InformedTrader::orderSize(AgentContext& context, Quantity room) const {
    if (config_.sizeTail == 0.0) {
        return config_.orderSize;
    }
    return std::min(
        drawParentSize(context.random(), config_.orderSize, config_.sizeTail, config_.maxPosition),
        room);
}

void InformedTrader::onStart(AgentContext& context) { context.wakeWithin(config_.interval); }

void InformedTrader::onWakeup(AgentContext& context, std::uint64_t /*tag*/) {
    const MarketSnapshot market = context.market();
    market_.update(market);
    const double estimate = belief_.look(context);

    const Ledger& ledger = context.ledger();
    const Quantity size = config_.orderSize;
    const auto ask = market_.bestAsk();
    const auto bid = market_.bestBid();
    // Room for buying and for selling within the position limit, counting orders in flight.
    const Quantity buyRoom =
        config_.maxPosition - ledger.position() - ledger.openQuantity(Side::Buy);
    const Quantity sellRoom =
        config_.maxPosition + ledger.position() - ledger.openQuantity(Side::Sell);
    if (market.phase != Phase::Continuous) {
        // Its immediate-or-cancel orders cannot wait for an auction; it trades after.
    } else if (ask && estimate - static_cast<double>(*ask) >= config_.threshold &&
               size <= buyRoom) {
        const Price limit = clampPrice(std::floor(estimate - config_.threshold),
                                       context.maxPrice());
        context.submitLimit(Side::Buy, limit, orderSize(context, buyRoom),
                            TimeInForce::ImmediateOrCancel);
    } else if (bid && static_cast<double>(*bid) - estimate >= config_.threshold &&
               size <= sellRoom) {
        const Price limit = clampPrice(std::ceil(estimate + config_.threshold),
                                       context.maxPrice());
        context.submitLimit(Side::Sell, limit, orderSize(context, sellRoom),
                            TimeInForce::ImmediateOrCancel);
    }
    context.wakeAfter(config_.interval);
}

} // namespace crowdbook

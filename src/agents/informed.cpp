#include "crowdbook/agents/informed.hpp"

#include <cmath>
#include <format>
#include <stdexcept>
#include <utility>

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
    if (market.phase != Phase::Continuous) {
        // Its immediate-or-cancel orders cannot wait for an auction; it trades after.
    } else if (ask && estimate - static_cast<double>(*ask) >= config_.threshold &&
        ledger.position() + ledger.openQuantity(Side::Buy) + size <= config_.maxPosition) {
        const Price limit = clampPrice(std::floor(estimate - config_.threshold),
                                       context.maxPrice());
        context.submitLimit(Side::Buy, limit, size, TimeInForce::ImmediateOrCancel);
    } else if (bid && static_cast<double>(*bid) - estimate >= config_.threshold &&
               ledger.position() - ledger.openQuantity(Side::Sell) - size >=
                   -config_.maxPosition) {
        const Price limit = clampPrice(std::ceil(estimate + config_.threshold),
                                       context.maxPrice());
        context.submitLimit(Side::Sell, limit, size, TimeInForce::ImmediateOrCancel);
    }
    context.wakeAfter(config_.interval);
}

} // namespace crowdbook

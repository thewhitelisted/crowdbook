#include "crowdbook/agents/probe.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

#include "crowdbook/math.hpp"

namespace crowdbook {

namespace {

// The weight of each new sample in a moving average that halves an old value's influence every
// `halfLife`, when sampled every `interval`.
double weightFor(Duration interval, Duration halfLife) {
    return 1.0 - math::exp2(-static_cast<double>(interval) / static_cast<double>(halfLife));
}

} // namespace

Probe::Probe(const ProbeConfig& config, Price referencePrice)
    : config_(config), market_(referencePrice) {
    if (config.interval <= 0 || config.fastHalfLife <= 0 ||
        config.fastHalfLife >= config.slowHalfLife || config.window <= 0) {
        throw std::invalid_argument("a probe needs a positive interval and window and "
                                    "0 < fast_half_life < slow_half_life");
    }
    if (!(config.offset >= 0.0) || !(config.threshold >= 0.0) ||
        !(config.imbalance >= 0.5 && config.imbalance < 1.0)) {
        throw std::invalid_argument("a probe needs an offset and a threshold that are not "
                                    "negative and an imbalance from 0.5 to below 1");
    }
    if (config.size < 1 || config.maxPosition < config.size || config.maxPosition > kMaxQuantity) {
        throw std::invalid_argument(
            std::format("a probe needs 1 <= size <= max_position <= {}", kMaxQuantity));
    }
    fastWeight_ = weightFor(config.interval, config.fastHalfLife);
    slowWeight_ = weightFor(config.interval, config.slowHalfLife);
}

void Probe::onStart(AgentContext& context) { context.wakeWithin(config_.interval); }

void Probe::onTopOfBook(AgentContext& /*context*/, const TopOfBook& top) { market_.update(top); }

void Probe::onTrade(AgentContext& context, const Trade& trade) {
    market_.update(trade);
    if (config_.style == ProbeStyle::Flow && !trade.auction) {
        flow_.emplace_back(context.now(),
                           trade.aggressorSide == Side::Buy ? trade.quantity : -trade.quantity);
    }
}

void Probe::forget(ClientOrderId id) {
    if (bid_ && bid_->id == id) {
        bid_.reset();
    }
    if (ask_ && ask_->id == id) {
        ask_.reset();
    }
}

void Probe::onFilled(AgentContext& /*context*/, const OrderFilled& event) {
    if (event.leavesQuantity == 0) {
        forget(event.clientOrderId);
    }
}

void Probe::onCancelled(AgentContext& /*context*/, const OrderCancelled& event) {
    forget(event.clientOrderId);
}

void Probe::onRejected(AgentContext& /*context*/, const OrderRejected& event) {
    forget(event.clientOrderId);
}

void Probe::quote(AgentContext& context, Side side, Price price) {
    std::optional<Quote>& resting = side == Side::Buy ? bid_ : ask_;
    if (resting && resting->price == price) {
        return;
    }
    if (resting) {
        context.cancel(resting->id);
        resting.reset();
    }
    const Ledger& ledger = context.ledger();
    const Quantity room =
        side == Side::Buy
            ? config_.maxPosition - ledger.position() - ledger.openQuantity(Side::Buy)
            : config_.maxPosition + ledger.position() - ledger.openQuantity(Side::Sell);
    if (room >= config_.size && price >= 1 && price <= context.maxPrice()) {
        resting = Quote{.id = context.submitLimit(side, price, config_.size), .price = price};
    }
}

void Probe::takeToward(AgentContext& context, Quantity target) {
    const Ledger& ledger = context.ledger();
    // Counting what is in flight, as if it all fills.
    const Quantity ifBought = ledger.position() + ledger.openQuantity(Side::Buy);
    const Quantity ifSold = ledger.position() - ledger.openQuantity(Side::Sell);
    if (target > ifBought && market_.bestAsk()) {
        context.submitLimit(Side::Buy, *market_.bestAsk(),
                            std::min(config_.size, target - ifBought),
                            TimeInForce::ImmediateOrCancel);
    } else if (target < ifSold && market_.bestBid()) {
        context.submitLimit(Side::Sell, *market_.bestBid(), std::min(config_.size, ifSold - target),
                            TimeInForce::ImmediateOrCancel);
    }
}

void Probe::onWakeup(AgentContext& context, std::uint64_t /*tag*/) {
    context.wakeAfter(config_.interval);
    market_.update(context.market());
    if (context.market().phase != Phase::Continuous) {
        return;
    }
    const double mid = market_.fairPrice();
    switch (config_.style) {
    case ProbeStyle::Maker:
        quote(context, Side::Buy, static_cast<Price>(std::floor(mid - config_.offset)));
        quote(context, Side::Sell, static_cast<Price>(std::ceil(mid + config_.offset)));
        return;
    case ProbeStyle::Trend:
    case ProbeStyle::Reversal: {
        if (fast_ && slow_) {
            *fast_ += fastWeight_ * (mid - *fast_);
            *slow_ += slowWeight_ * (mid - *slow_);
        } else {
            fast_ = mid;
            slow_ = mid;
        }
        const double lead = (*fast_ - *slow_) * (config_.style == ProbeStyle::Trend ? 1 : -1);
        if (lead > config_.threshold) {
            target_ = config_.maxPosition;
        } else if (lead < -config_.threshold) {
            target_ = -config_.maxPosition;
        }
        break;
    }
    case ProbeStyle::Flow: {
        while (!flow_.empty() && flow_.front().first <= context.now() - config_.window) {
            flow_.pop_front();
        }
        Quantity bought = 0;
        Quantity total = 0;
        for (const auto& [time, signedQuantity] : flow_) {
            bought += std::max<Quantity>(signedQuantity, 0);
            total += signedQuantity < 0 ? -signedQuantity : signedQuantity;
        }
        if (total > 0) {
            const double share = static_cast<double>(bought) / static_cast<double>(total);
            if (share >= config_.imbalance) {
                target_ = config_.maxPosition;
            } else if (share <= 1.0 - config_.imbalance) {
                target_ = -config_.maxPosition;
            }
        }
        break;
    }
    }
    takeToward(context, target_);
}

} // namespace crowdbook

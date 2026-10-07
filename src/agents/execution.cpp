#include "crowdbook/agents/execution.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

namespace crowdbook {

Quantity drawParentSize(Random& random, Quantity minParent, double parentTail,
                        Quantity maxParent) {
    // Inverting the distribution: 1 - uniform() is in (0, 1], so the size is at least minParent.
    const double size = static_cast<double>(minParent) *
                        std::pow(1.0 - random.uniform(), -1.0 / parentTail);
    return size >= static_cast<double>(maxParent) ? maxParent : static_cast<Quantity>(size);
}

ExecutionTrader::ExecutionTrader(const ExecutionConfig& config) : config_(config) {
    if (config.minParent < 1 || config.maxParent < config.minParent ||
        config.maxParent > kMaxQuantity) {
        throw std::invalid_argument(std::format(
            "execution needs 1 <= min_parent <= max_parent <= {}", kMaxQuantity));
    }
    if (!(config.parentTail > 0.0) || !std::isfinite(config.parentTail)) {
        throw std::invalid_argument("execution parent_tail must be positive");
    }
    if (config.pause < 0 || config.interval <= 0) {
        throw std::invalid_argument(
            "execution needs a pause that is not negative and a positive interval");
    }
    if (config.childSize < 1 || config.childSize > kMaxQuantity) {
        throw std::invalid_argument(
            std::format("execution child_size must be between 1 and {}", kMaxQuantity));
    }
    if (!(config.participation > 0.0) || !(config.participation < 1.0)) {
        throw std::invalid_argument("execution participation must be between 0 and 1");
    }
}

void ExecutionTrader::onStart(AgentContext& context) { pauseThenStart(context); }

void ExecutionTrader::pauseThenStart(AgentContext& context) const {
    if (config_.pause == 0) {
        context.wakeAfter(0);
        return;
    }
    const double mean = static_cast<double>(config_.pause);
    const double wait = std::min(context.random().exponential(1.0) * mean,
                                 static_cast<double>(kMaxDuration));
    context.wakeAfter(static_cast<Duration>(wait));
}

void ExecutionTrader::start(AgentContext& context) {
    parent_ = ++parentsStarted_;
    Random& random = context.random();
    side_ = random.below(2) == 0 ? Side::Buy : Side::Sell;
    size_ = drawParentSize(random, config_.minParent, config_.parentTail, config_.maxParent);
    unsent_ = size_;
    abandoned_ = false;
    startVolume_ = context.market().volume;
}

Quantity ExecutionTrader::due(AgentContext& context) const {
    if (config_.style == ExecutionStyle::Twap) {
        return std::min(config_.childSize, unsent_);
    }
    const Quantity traded = context.market().volume - startVolume_;
    const auto allowed =
        static_cast<Quantity>(config_.participation * static_cast<double>(traded));
    const Quantity sent = size_ - unsent_;
    return std::clamp<Quantity>(allowed - sent, 0, std::min(config_.childSize, unsent_));
}

void ExecutionTrader::onWakeup(AgentContext& context, std::uint64_t /*tag*/) {
    if (parent_ == 0) {
        start(context);
    } else if (unsent_ == 0 && context.ledger().orders().empty()) {
        // Every child is filled or cancelled for good: the parent is done.
        parent_ = 0;
        pauseThenStart(context);
        return;
    }
    if (const Quantity lots = due(context); lots > 0) {
        unsent_ -= lots;
        context.submit(
            {.side = side_, .type = OrderType::Market, .quantity = lots, .parent = parent_});
    }
    context.wakeAfter(config_.interval);
}

void ExecutionTrader::onRejected(AgentContext& /*context*/, const OrderRejected& /*event*/) {
    unsent_ = 0; // give up on the rest of the parent
    abandoned_ = true;
}

void ExecutionTrader::onCancelled(AgentContext& /*context*/, const OrderCancelled& event) {
    // A child the book could not fill in full; its lots go back to the parent, unless the agent
    // has given up on it.
    if (parent_ != 0 && !abandoned_) {
        unsent_ += event.quantity;
    }
}

} // namespace crowdbook

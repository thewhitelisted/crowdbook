#include "crowdbook/agents/zero_intelligence.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <stdexcept>

#include "crowdbook/math.hpp"

namespace crowdbook {

namespace {

constexpr std::uint64_t kNextOrder = 0; // wakeup tag; client order ids start at 1

Side randomSide(Random& random) { return random.below(2) == 0 ? Side::Buy : Side::Sell; }

// A random waiting time in nanoseconds: at least one, and at most kMaxDuration, so that a tiny
// configured rate cannot overflow the conversion or the clock.
Duration secondsToDuration(double seconds) {
    const double nanoseconds = std::min(seconds * static_cast<double>(kSecond),
                                        static_cast<double>(kMaxDuration));
    return std::max<Duration>(static_cast<Duration>(nanoseconds), 1);
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
    if (config.maxOffset < 1 || config.maxOffset > kMaxPrice) {
        throw std::invalid_argument(
            std::format("zero_intelligence max_offset must be between 1 and {}", kMaxPrice));
    }
    if (config.minSize < 1 || config.minSize > config.maxSize || config.maxSize > kMaxQuantity) {
        throw std::invalid_argument(std::format(
            "zero_intelligence sizes need 1 <= min_size <= max_size <= {}", kMaxQuantity));
    }
    if (!(config.activityResponse >= 0.0) || !(config.volatilityResponse >= 0.0) ||
        config.activityMemory <= 0 || config.activityBaseline < config.activityMemory) {
        throw std::invalid_argument(
            "zero_intelligence needs activity_response and volatility_response >= 0 and "
            "0 < activity_memory <= activity_baseline");
    }
}

void ZeroIntelligenceTrader::Average::add(double value, double keep) noexcept {
    weightedSum = weightedSum * keep + value * (1.0 - keep);
    weight = weight * keep + (1.0 - keep);
}

void ZeroIntelligenceTrader::observeMarket(const MarketSnapshot& market, Timestamp now) {
    if (config_.activityResponse == 0.0 && config_.volatilityResponse == 0.0) {
        return;
    }
    const std::optional<double> mid =
        market.bid && market.ask
            ? std::optional{static_cast<double>(market.bid->price + market.ask->price) / 2.0}
            : std::nullopt;
    if (lastSeenAt_ >= 0 && now > lastSeenAt_) {
        const double seconds = static_cast<double>(now - lastSeenAt_) / kSecond;
        const auto keep = [seconds](Duration window) {
            return math::exp(-seconds * kSecond / static_cast<double>(window));
        };
        if (config_.activityResponse > 0.0) {
            const double rate = static_cast<double>(market.trades - lastSeenTrades_) / seconds;
            recent_.add(rate, keep(config_.activityMemory));
            usual_.add(rate, keep(config_.activityBaseline));
            if (recent_.value() > 0.0 && usual_.value() > 0.0) {
                pace_ = std::clamp(
                    math::pow(recent_.value() / usual_.value(), config_.activityResponse), 0.1,
                    10.0);
            }
        }
        if (config_.volatilityResponse > 0.0 && mid && lastSeenMid_) {
            const double move = *mid - *lastSeenMid_;
            recentVariance_.add(move * move / seconds, keep(config_.activityMemory));
            usualVariance_.add(move * move / seconds, keep(config_.activityBaseline));
            if (recentVariance_.value() > 0.0 && usualVariance_.value() > 0.0) {
                // Variances, so half the exponent gives the ratio of volatilities.
                stretch_ = std::clamp(math::pow(recentVariance_.value() / usualVariance_.value(),
                                               config_.volatilityResponse / 2.0),
                                      0.25, 4.0);
            }
        }
    }
    lastSeenAt_ = now;
    lastSeenTrades_ = market.trades;
    if (mid) {
        lastSeenMid_ = mid;
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

    const MarketSnapshot market = context.market();
    market_.update(market);
    observeMarket(market, context.now());
    // Picking limit or market in proportion to its rate, with one exponential timer for both, is
    // the same as running two independent Poisson processes.
    if (context.random().uniform() * orderRate() < config_.limitRate) {
        if (market.phase != Phase::Closed) {
            sendLimit(context);
        }
    } else if (market.phase == Phase::Continuous) {
        sendMarket(context);
    } else if (isAuction(market.phase)) {
        sendAuctionLimit(context, market);
    }
    scheduleNextOrder(context);
}

double ZeroIntelligenceTrader::orderRate() const noexcept {
    return config_.limitRate + config_.marketRate;
}

void ZeroIntelligenceTrader::scheduleNextOrder(AgentContext& context) const {
    const double rate = orderRate() * pace_ * config_.activity.at(context.now());
    context.wakeAt(context.now() + secondsToDuration(context.random().exponential(rate)),
                   kNextOrder);
}

void ZeroIntelligenceTrader::sendLimit(AgentContext& context) const {
    Random& random = context.random();
    const Side side = randomSide(random);
    const Price widest = std::max<Price>(
        1, std::llround(static_cast<double>(config_.maxOffset) * stretch_));
    const Price offset = random.uniformInt(1, widest);
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

void ZeroIntelligenceTrader::sendAuctionLimit(AgentContext& context,
                                              const MarketSnapshot& market) const {
    Random& random = context.random();
    const Side side = randomSide(random);
    const Quantity size = random.uniformInt(config_.minSize, config_.maxSize);
    const Price offset = random.uniformInt(1, config_.maxOffset);
    const Price anchor = market.indicative
                             ? market.indicative->price
                             : static_cast<Price>(std::llround(market_.fairPrice()));
    const Price price = side == Side::Buy ? anchor + offset : anchor - offset;
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

#include "crowdbook/agents/market_maker.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include <utility>

#include "crowdbook/math.hpp"

namespace crowdbook {

namespace {
constexpr int kMaxLevels = 50;
constexpr double kMaxGrowth = 1000.0;
} // namespace

Quotes avellanedaStoikovQuotes(const MarketMakerConfig& config, double fairPrice,
                               Quantity inventory) {
    const double gamma = config.riskAversion;
    const double tau = static_cast<double>(config.horizon) / static_cast<double>(kSecond);
    const double risk = gamma * config.volatility * config.volatility * tau;
    const double reservation = fairPrice - static_cast<double>(inventory) * risk;
    const double halfSpread = risk / 2.0 + math::log(1.0 + gamma / config.intensity) / gamma;

    Quotes quotes;
    if (inventory + config.quoteSize <= config.maxInventory) {
        const double bid = std::floor(reservation - halfSpread);
        if (bid >= 1.0) {
            quotes.bid = clampPrice(bid);
        }
    }
    if (inventory - config.quoteSize >= -config.maxInventory) {
        quotes.ask = clampPrice(std::ceil(reservation + halfSpread));
    }
    return quotes;
}

MarketMaker::MarketMaker(const MarketMakerConfig& config, Price referencePrice,
                         std::shared_ptr<Fundamental> fundamental)
    : config_(config), market_(referencePrice) {
    if (!(config.riskAversion > 0.0) || !(config.intensity > 0.0) ||
        !(config.volatility >= 0.0) || config.horizon < 0) {
        throw std::invalid_argument("market_maker needs risk_aversion > 0, intensity > 0, "
                                    "volatility >= 0 and a horizon that is not negative");
    }
    if (config.quoteSize < 1 || config.maxInventory < config.quoteSize ||
        config.maxInventory > kMaxQuantity) {
        throw std::invalid_argument(std::format(
            "market_maker needs 1 <= quote_size <= max_inventory <= {}", kMaxQuantity));
    }
    if (config.requoteInterval <= 0) {
        throw std::invalid_argument("market_maker requote_interval must be positive");
    }
    if (!(config.fairValueWeight > 0.0) || config.fairValueWeight > 1.0) {
        throw std::invalid_argument("market_maker fair_value_weight must be in (0, 1]");
    }
    if (config.quietInterval < 0 ||
        (config.quietFrom && (*config.quietFrom < 0 || config.quietUntil < *config.quietFrom))) {
        throw std::invalid_argument("market_maker's quiet stretch must start at 0 or later, end "
                                    "no sooner, and have an interval that is not negative");
    }
    if (!(config.requoteThreshold >= 0.0)) {
        throw std::invalid_argument("market_maker requote_threshold must not be negative");
    }
    if (config.levels < 1 || config.levels > kMaxLevels || config.levelSpacing < 1 ||
        config.levelSpacing > kMaxPrice || !(config.levelGrowth > 0.0) ||
        !(config.levelGrowth <= kMaxGrowth)) {
        throw std::invalid_argument(std::format(
            "market_maker needs 1 to {} levels, a level_spacing from 1 tick to {} and a "
            "level_growth above 0 and at most {}",
            kMaxLevels, kMaxPrice, kMaxGrowth));
    }
    if (config.valueBelief) {
        belief_.emplace(*config.valueBelief, std::move(fundamental));
    }
    bids_.resize(static_cast<std::size_t>(config.levels));
    asks_.resize(static_cast<std::size_t>(config.levels));
}

Quantity MarketMaker::levelSize(int level) const noexcept {
    // Kept within 1 and max_inventory before rounding, so that a size too large for a whole
    // number cannot wrap round.
    const double size = std::clamp(static_cast<double>(config_.quoteSize) *
                                       std::pow(config_.levelGrowth, level),
                                   1.0, static_cast<double>(config_.maxInventory));
    return static_cast<Quantity>(std::llround(size));
}

void MarketMaker::onStart(AgentContext& context) {
    requote(context);
    context.wakeWithin(config_.requoteInterval);
}

void MarketMaker::onWakeup(AgentContext& context, std::uint64_t /*tag*/) {
    requote(context);
    context.wakeAfter(config_.requoteInterval);
}

void MarketMaker::onFilled(AgentContext& context, const OrderFilled& /*event*/) {
    requote(context, /*afterFill=*/true);
}

void MarketMaker::onRejected(AgentContext& context, const OrderRejected& event) {
    // The order kept its old price; remember that, so the next requote asks again.
    for (auto* side : {&bids_, &asks_}) {
        for (std::optional<Quote>& quote : *side) {
            if (event.request == RequestKind::Modify && quote &&
                quote->clientOrderId == event.clientOrderId) {
                if (const OwnOrder* live = context.ledger().find(event.clientOrderId)) {
                    quote->price = live->price;
                }
                quotedFair_.reset(); // whatever its threshold, the next requote asks again
            }
        }
    }
}

void MarketMaker::onTopOfBook(AgentContext& /*context*/, const TopOfBook& top) {
    market_.update(top);
}

void MarketMaker::onTrade(AgentContext& /*context*/, const Trade& trade) {
    market_.update(trade);
    const auto price = static_cast<double>(trade.price);
    if (tradeAverage_) {
        *tradeAverage_ += config_.fairValueWeight * (price - *tradeAverage_);
    } else {
        tradeAverage_ = price;
    }
}

double MarketMaker::fairPrice() const noexcept {
    return tradeAverage_.value_or(market_.fairPrice());
}

void MarketMaker::onPhase(AgentContext& context, const PhaseChanged& phase) {
    phase_ = phase.phase;
    requote(context);
}

void MarketMaker::requote(AgentContext& context, bool afterFill) {
    if (phase_ != Phase::Continuous) {
        return;
    }
    const Timestamp now = context.now();
    if (config_.quietFrom && now >= *config_.quietFrom && now < config_.quietUntil) {
        if (config_.quietInterval == 0) {
            for (int level = 0; level < config_.levels; ++level) {
                const auto at = static_cast<std::size_t>(level);
                maintain(context, bids_[at], Side::Buy, std::nullopt, levelSize(level));
                maintain(context, asks_[at], Side::Sell, std::nullopt, levelSize(level));
            }
            return;
        }
        if (quotedAt_ && now - *quotedAt_ < config_.quietInterval) {
            return;
        }
        quotedAt_ = now;
    }
    const double fair = belief_ ? belief_->look(context) : fairPrice();
    if (config_.requoteThreshold > 0.0 && !afterFill && quotedFair_ &&
        std::abs(fair - *quotedFair_) < config_.requoteThreshold && standing(context)) {
        return;
    }
    quotedFair_ = fair;
    Quotes target = avellanedaStoikovQuotes(config_, fair, context.ledger().position());
    if (config_.postOnly) {
        if (target.bid && market_.bestAsk()) {
            target.bid = std::min(*target.bid, *market_.bestAsk() - 1);
        }
        if (target.ask && market_.bestBid()) {
            target.ask = std::max(*target.ask, *market_.bestBid() + 1);
        }
    }
    // Within the exchange's prices, with room for an ask above the bid. A post-only quote pushed
    // against the limit would trade, and is left out instead.
    const Price highest = context.maxPrice();
    if (target.bid) {
        target.bid = std::min(*target.bid, highest - 1);
        if (*target.bid < 1 ||
            (config_.postOnly && market_.bestAsk() && *target.bid >= *market_.bestAsk())) {
            target.bid.reset();
        }
    }
    if (target.ask) {
        target.ask = std::min(*target.ask, highest);
        if (config_.postOnly && market_.bestBid() && *target.ask <= *market_.bestBid()) {
            target.ask.reset();
        }
    }
    // The ladder: each level behind the touch quote, quoted while filling it and every level
    // nearer would keep the position within max_inventory.
    const Quantity position = context.ledger().position();
    Quantity bought = 0;
    Quantity sold = 0;
    for (int level = 0; level < config_.levels; ++level) {
        const auto at = static_cast<std::size_t>(level);
        const Quantity size = levelSize(level);
        const Price behind = static_cast<Price>(level) * config_.levelSpacing;
        bought += size;
        sold += size;
        std::optional<Price> bid;
        if (target.bid && *target.bid - behind >= 1 &&
            position + bought <= config_.maxInventory) {
            bid = *target.bid - behind;
        }
        std::optional<Price> ask;
        if (target.ask && *target.ask + behind <= highest &&
            position - sold >= -config_.maxInventory) {
            ask = *target.ask + behind;
        }
        maintain(context, bids_[at], Side::Buy, bid, size);
        maintain(context, asks_[at], Side::Sell, ask, size);
    }
}

bool MarketMaker::standing(const AgentContext& context) const {
    for (const auto* side : {&bids_, &asks_}) {
        for (std::size_t level = 0; level < side->size(); ++level) {
            const std::optional<Quote>& quote = (*side)[level];
            if (!quote) {
                continue; // left out at its last requote, as the limits or the book asked
            }
            const OwnOrder* live = context.ledger().find(quote->clientOrderId);
            if (live == nullptr || live->cancelRequested ||
                live->leaves != levelSize(static_cast<int>(level))) {
                return false;
            }
        }
    }
    return true;
}

void MarketMaker::maintain(AgentContext& context, std::optional<Quote>& quote, Side side,
                           std::optional<Price> price, Quantity size) const {
    const OwnOrder* live = quote ? context.ledger().find(quote->clientOrderId) : nullptr;
    if (live == nullptr || live->cancelRequested) {
        quote.reset(); // filled, cancelled, or already on its way out
        live = nullptr;
    }

    if (!price) {
        if (quote) {
            context.cancel(quote->clientOrderId);
            quote.reset();
        }
    } else if (!quote) {
        const TimeInForce timeInForce =
            config_.postOnly ? TimeInForce::PostOnly : TimeInForce::GoodTillCancel;
        quote = Quote{
            .clientOrderId = context.submitLimit(side, *price, size, timeInForce),
            .price = *price};
    } else if (quote->price != *price || live->leaves != size) {
        context.modify(quote->clientOrderId, *price, size);
        quote->price = *price;
    }
}

} // namespace crowdbook

#include "crowdbook/agents/market_maker.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

namespace crowdbook {

Quotes avellanedaStoikovQuotes(const MarketMakerConfig& config, double fairPrice,
                               Quantity inventory) {
    const double gamma = config.riskAversion;
    const double tau = static_cast<double>(config.horizon) / static_cast<double>(kSecond);
    const double risk = gamma * config.volatility * config.volatility * tau;
    const double reservation = fairPrice - static_cast<double>(inventory) * risk;
    const double halfSpread = risk / 2.0 + std::log(1.0 + gamma / config.intensity) / gamma;

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

MarketMaker::MarketMaker(const MarketMakerConfig& config, Price referencePrice)
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
    requote(context);
}

void MarketMaker::onRejected(AgentContext& context, const OrderRejected& event) {
    // The order kept its old price; remember that, so the next requote asks again.
    for (std::optional<Quote>* quote : {&bid_, &ask_}) {
        if (event.request == RequestKind::Modify && *quote &&
            (*quote)->clientOrderId == event.clientOrderId) {
            if (const OwnOrder* live = context.ledger().find(event.clientOrderId)) {
                (*quote)->price = live->price;
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

void MarketMaker::requote(AgentContext& context) {
    Quotes target = avellanedaStoikovQuotes(config_, fairPrice(), context.ledger().position());
    if (config_.postOnly) {
        if (target.bid && market_.bestAsk()) {
            target.bid = std::min(*target.bid, *market_.bestAsk() - 1);
        }
        if (target.ask && market_.bestBid()) {
            target.ask = std::max(*target.ask, *market_.bestBid() + 1);
        }
    }
    maintain(context, bid_, Side::Buy, target.bid);
    maintain(context, ask_, Side::Sell, target.ask);
}

void MarketMaker::maintain(AgentContext& context, std::optional<Quote>& quote, Side side,
                           std::optional<Price> price) const {
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
            .clientOrderId = context.submitLimit(side, *price, config_.quoteSize, timeInForce),
            .price = *price};
    } else if (quote->price != *price || live->leaves != config_.quoteSize) {
        context.modify(quote->clientOrderId, *price, config_.quoteSize);
        quote->price = *price;
    }
}

} // namespace crowdbook

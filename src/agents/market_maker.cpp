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
    if (!(config.flowImpact >= 0.0) || !std::isfinite(config.flowImpact) ||
        (config.flowMemory && *config.flowMemory <= 0) ||
        (config.takeThrough && (!(*config.takeThrough >= 0.0) ||
                                !std::isfinite(*config.takeThrough)))) {
        throw std::invalid_argument("market_maker needs a flow_impact of 0 or more, a positive "
                                    "flow_memory and a take_through of 0 or more");
    }
    if (config.valueBelief) {
        belief_.emplace(*config.valueBelief, std::move(fundamental));
    }
    bids_.resize(static_cast<std::size_t>(config.levels));
    asks_.resize(static_cast<std::size_t>(config.levels));
    targetBids_.resize(static_cast<std::size_t>(config.levels));
    targetAsks_.resize(static_cast<std::size_t>(config.levels));
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

void MarketMaker::onFilled(AgentContext& context, const OrderFilled& event) {
    // Its own takes print as aggressive volume too, and it learns nothing from them: a take's
    // fill, which reaches it before the trade it prints, marks that many lots of the next trades
    // on its side as its own.
    if (config_.flowImpact > 0.0 && std::ranges::contains(takes_, event.clientOrderId)) {
        (event.side == Side::Buy ? ownBought_ : ownSold_) += event.quantity;
    }
    // A fill of a quote at the best price reaches it before the book does: those lots are gone.
    const std::optional<Price> best =
        event.side == Side::Buy ? market_.bestBid() : market_.bestAsk();
    if (event.liquidity == Liquidity::Maker && best == event.price) {
        Quantity& shown = event.side == Side::Buy ? topBidSize_ : topAskSize_;
        shown = std::max<Quantity>(shown - event.quantity, 0);
    }
    requote(context, /*afterFill=*/true);
}

void MarketMaker::onCancelled(AgentContext& context, const OrderCancelled& event) {
    // The rest of a take is done: it quoted as if all of it would fill, so it quotes afresh, but
    // takes nothing more until its timer, so that takes cannot follow one another without time
    // passing.
    if (std::ranges::contains(takes_, event.clientOrderId)) {
        requote(context, /*afterFill=*/true, /*mayTake=*/false);
    }
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
    topBidSize_ = top.bid ? top.bid->quantity : 0;
    topAskSize_ = top.ask ? top.ask->quantity : 0;
}

void MarketMaker::onTrade(AgentContext& context, const Trade& trade) {
    market_.update(trade);
    if (config_.flowImpact > 0.0 && !trade.auction) {
        Quantity& own = trade.aggressorSide == Side::Buy ? ownBought_ : ownSold_;
        const Quantity mine = std::min(own, trade.quantity);
        own -= mine;
        impact_ = flowImpact(context.now()) +
                  config_.flowImpact * static_cast<double>(trade.quantity - mine) *
                      (trade.aggressorSide == Side::Buy ? 1.0 : -1.0);
        impactAt_ = context.now();
    }
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

double MarketMaker::flowImpact(Timestamp now) const noexcept {
    if (!config_.flowMemory || impact_ == 0.0) {
        return impact_;
    }
    return impact_ * math::exp(-static_cast<double>(now - impactAt_) /
                               static_cast<double>(*config_.flowMemory));
}

void MarketMaker::onPhase(AgentContext& context, const PhaseChanged& phase) {
    phase_ = phase.phase;
    requote(context);
}

void MarketMaker::requote(AgentContext& context, bool afterFill, bool mayTake) {
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
            quotedFair_.reset(); // out of the book: it quotes afresh when the stretch ends
            return;
        }
        if (quotedAt_ && now - *quotedAt_ < config_.quietInterval) {
            return;
        }
        quotedAt_ = now;
    }
    double fair = belief_ ? belief_->look(context) : fairPrice();
    if (config_.flowImpact > 0.0) {
        fair += flowImpact(now);
    }
    // As if every take still open fills: the quotes leave room for it.
    const Quantity position = takeThrough(context, fair, mayTake);
    if (config_.requoteThreshold > 0.0 && !afterFill && quotedFair_ &&
        std::abs(fair - *quotedFair_) < config_.requoteThreshold && standing(context)) {
        return;
    }
    quotedFair_ = fair;
    Quotes target = avellanedaStoikovQuotes(config_, fair, position);
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
    Quantity bought = 0;
    Quantity sold = 0;
    std::vector<std::optional<Price>>& bids = targetBids_;
    std::vector<std::optional<Price>>& asks = targetAsks_;
    for (int level = 0; level < config_.levels; ++level) {
        const auto at = static_cast<std::size_t>(level);
        const Quantity size = levelSize(level);
        const Price behind = static_cast<Price>(level) * config_.levelSpacing;
        bought += size;
        sold += size;
        bids[at].reset();
        asks[at].reset();
        if (target.bid && *target.bid - behind >= 1 &&
            position + bought <= config_.maxInventory) {
            bids[at] = *target.bid - behind;
        }
        if (target.ask && *target.ask + behind <= highest &&
            position - sold >= -config_.maxInventory) {
            asks[at] = *target.ask + behind;
        }
    }
    // Requests reach the exchange in the order sent, and one that meets its own quote there is
    // cancelled: when its new bid would reach one of its asks as they stand, the asks move
    // first, and the bids after. Only a maker that reads flow, whose price jumps by whole
    // spreads, does this, so that every other runs as it always has.
    bool asksFirst = false;
    if (bids[0] && (config_.flowImpact > 0.0 || config_.takeThrough)) {
        for (const std::optional<Quote>& quote : asks_) {
            asksFirst = asksFirst || (quote && quote->price <= *bids[0]);
        }
    }
    if (asksFirst) {
        for (int level = 0; level < config_.levels; ++level) {
            const auto at = static_cast<std::size_t>(level);
            maintain(context, asks_[at], Side::Sell, asks[at], levelSize(level));
        }
    }
    for (int level = 0; level < config_.levels; ++level) {
        const auto at = static_cast<std::size_t>(level);
        maintain(context, bids_[at], Side::Buy, bids[at], levelSize(level));
        if (!asksFirst) {
            maintain(context, asks_[at], Side::Sell, asks[at], levelSize(level));
        }
    }
}

Quantity MarketMaker::takeThrough(AgentContext& context, double fair, bool mayTake) {
    const Ledger& ledger = context.ledger();
    if (!config_.takeThrough) {
        return ledger.position();
    }
    // Takes still open count as filled; a take that is done drops out.
    std::erase_if(takes_, [&](ClientOrderId id) { return ledger.find(id) == nullptr; });
    const auto open = [&](Side side) {
        Quantity lots = 0;
        for (const ClientOrderId id : takes_) {
            const OwnOrder* take = ledger.find(id);
            if (take->side == side) {
                lots += take->leaves;
            }
        }
        return lots;
    };
    const double through = *config_.takeThrough;
    const Price highest = context.maxPrice();
    // The exchange stops a take at the first of its own quotes it reaches, so it first cancels
    // those the take could reach; they arrive first, as messages on a link never overtake. Room is
    // as the exchange counts it: the position were every open order on that side to fill.
    const auto take = [&](Side side, Price price, Quantity room) {
        if (room <= 0) {
            return;
        }
        for (std::optional<Quote>& quote : side == Side::Buy ? asks_ : bids_) {
            const OwnOrder* live = quote ? ledger.find(quote->clientOrderId) : nullptr;
            // At its price in the book or the one a modify still on its way asks for.
            const auto reaches = [&](Price at) {
                return side == Side::Buy ? at <= price : at >= price;
            };
            if (live != nullptr && !live->cancelRequested &&
                (reaches(live->price) || reaches(quote->price))) {
                context.cancel(quote->clientOrderId);
                quote.reset();
            }
        }
        takes_.push_back(context.submitLimit(side, price, room, TimeInForce::ImmediateOrCancel));
        quotedFair_.reset(); // whatever its threshold, it quotes afresh around the take
    };
    // Someone else's quote at the far side's best price: the book shows more there than its own.
    // Its own there are every order the exchange holds for it at that price, a cancel on its way
    // or not.
    const auto others = [&](Side side, Price best, Quantity shown) {
        Quantity own = 0;
        for (const auto& [id, order] : ledger.orders()) {
            if (order.acknowledged && order.side == side && order.price == best) {
                own += order.leaves;
            }
        }
        return shown > own;
    };
    if (mayTake && market_.bestAsk() && fair - through > static_cast<double>(*market_.bestAsk()) &&
        others(Side::Sell, *market_.bestAsk(), topAskSize_)) {
        take(Side::Buy, clampPrice(std::floor(fair - through), highest),
             config_.maxInventory - ledger.position() - ledger.openQuantity(Side::Buy));
    } else if (mayTake && market_.bestBid() &&
               fair + through < static_cast<double>(*market_.bestBid()) &&
               others(Side::Buy, *market_.bestBid(), topBidSize_)) {
        take(Side::Sell, clampPrice(std::ceil(fair + through), highest),
             config_.maxInventory + ledger.position() - ledger.openQuantity(Side::Sell));
    }
    return ledger.position() + open(Side::Buy) - open(Side::Sell);
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

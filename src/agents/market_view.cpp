#include "crowdbook/agents/market_view.hpp"

namespace crowdbook {

void MarketView::update(const TopOfBook& top) noexcept {
    bid_ = top.bid ? std::optional{top.bid->price} : std::nullopt;
    ask_ = top.ask ? std::optional{top.ask->price} : std::nullopt;
}

void MarketView::update(const Trade& trade) noexcept { lastTrade_ = trade.price; }

double MarketView::fairPrice() const noexcept {
    if (bid_ && ask_) {
        return static_cast<double>(*bid_ + *ask_) / 2.0;
    }
    return static_cast<double>(lastTrade_.value_or(reference_));
}

} // namespace crowdbook

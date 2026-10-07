#pragma once

#include <optional>

#include "crowdbook/messages.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// What an agent knows about the market from public data alone: the best bid and ask it last heard
// about and the last trade price. Feed it every TopOfBook and Trade the agent receives.
class MarketView {
public:
    explicit MarketView(Price referencePrice) noexcept : reference_(referencePrice) {}

    void update(const TopOfBook& top) noexcept;
    void update(const Trade& trade) noexcept;

    [[nodiscard]] std::optional<Price> bestBid() const noexcept { return bid_; }
    [[nodiscard]] std::optional<Price> bestAsk() const noexcept { return ask_; }
    [[nodiscard]] std::optional<Price> lastTrade() const noexcept { return lastTrade_; }
    // The mid price if both sides are quoted, else the last trade price, else the reference
    // price.
    [[nodiscard]] double fairPrice() const noexcept;

private:
    Price reference_;
    std::optional<Price> bid_;
    std::optional<Price> ask_;
    std::optional<Price> lastTrade_;
};

} // namespace crowdbook

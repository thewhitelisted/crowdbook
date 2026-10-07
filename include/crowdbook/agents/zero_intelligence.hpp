#pragma once

#include <cstdint>

#include "crowdbook/agent.hpp"
#include "crowdbook/agents/market_view.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

struct ZeroIntelligenceConfig {
    double limitRate = 2.0;  // limit orders per second
    double marketRate = 0.5; // market orders per second
    // Each resting order is cancelled at this rate per second, so one that never fills rests for
    // 1 / cancelRate seconds on average. 0 leaves orders resting until they fill.
    double cancelRate = 0.2;
    Price maxOffset = 10; // limit orders go 1 to maxOffset ticks inside the opposite best quote
    Quantity minSize = 1;
    Quantity maxSize = 10;
};

// A zero-intelligence trader after Farmer, Patelli and Zovko (2005). It sends limit and market
// orders at random times (Poisson processes with the configured rates), on random sides and with
// random sizes, and cancels each of its resting orders after a random, exponentially distributed
// lifetime. A limit price is drawn uniformly from 1 to maxOffset ticks inside the opposite best
// quote, so it never crosses the book as the trader last saw it. With no strategy at all, such
// order flow already explains much of how spreads and volatility vary across real stocks.
//
// Cancelling per order, rather than at a fixed rate per trader, matters: total cancellations then
// grow with the number of resting orders, so the book settles at a steady size. With a fixed rate
// the book grows without limit, and the orders piling up around the early prices pin the price.
class ZeroIntelligenceTrader final : public Agent {
public:
    // Throws std::invalid_argument for a negative rate, limit and market rates that are both zero,
    // a maxOffset below 1 or sizes that do not satisfy 1 <= minSize <= maxSize.
    ZeroIntelligenceTrader(const ZeroIntelligenceConfig& config, Price referencePrice);

    // It only looks at the market when it acts, so it reads snapshots instead of the stream.
    [[nodiscard]] MarketDataMode marketData() const noexcept override {
        return MarketDataMode::Snapshot;
    }
    void onStart(AgentContext& context) override;
    // Tag 0 is the timer for its next order; any other tag is the client order id of a resting
    // order whose lifetime is over.
    void onWakeup(AgentContext& context, std::uint64_t tag) override;

private:
    [[nodiscard]] double orderRate() const noexcept;
    void scheduleNextOrder(AgentContext& context) const;
    void sendLimit(AgentContext& context) const;
    void sendMarket(AgentContext& context) const;

    ZeroIntelligenceConfig config_;
    MarketView market_;
};

} // namespace crowdbook

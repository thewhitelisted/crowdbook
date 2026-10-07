#pragma once

#include <cstdint>

#include "crowdbook/agent.hpp"
#include "crowdbook/agents/market_view.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

struct ZeroIntelligenceConfig {
    double limitRate = 2.0;  // limit orders per second
    double marketRate = 0.5; // market orders per second
    double cancelRate = 1.0; // cancellations of its own orders per second
    Price maxOffset = 10;    // limit orders go 1 to maxOffset ticks inside the opposite best quote
    Quantity minSize = 1;
    Quantity maxSize = 10;
};

// A zero-intelligence trader after Farmer, Patelli and Zovko (2005). It sends limit orders, market
// orders and cancellations of its own orders at random times (Poisson processes with the
// configured rates), on random sides and with random sizes. A limit price is drawn uniformly from
// 1 to maxOffset ticks inside the opposite best quote, so it never crosses the book as the trader
// last saw it. With no strategy at all, such order flow already explains much of how spreads and
// volatility vary across real stocks.
class ZeroIntelligenceTrader final : public Agent {
public:
    // Throws std::invalid_argument for a negative rate, rates that are all zero, a maxOffset below
    // 1 or sizes that do not satisfy 1 <= minSize <= maxSize.
    ZeroIntelligenceTrader(const ZeroIntelligenceConfig& config, Price referencePrice);

    // It only looks at the market when it acts, so it reads snapshots instead of the stream.
    [[nodiscard]] MarketDataMode marketData() const noexcept override {
        return MarketDataMode::Snapshot;
    }
    void onStart(AgentContext& context) override;
    void onWakeup(AgentContext& context, std::uint64_t tag) override;

private:
    [[nodiscard]] double totalRate() const noexcept;
    void scheduleNext(AgentContext& context) const;
    void sendLimit(AgentContext& context) const;
    void sendMarket(AgentContext& context) const;
    static void cancelOne(AgentContext& context);

    ZeroIntelligenceConfig config_;
    MarketView market_;
};

} // namespace crowdbook

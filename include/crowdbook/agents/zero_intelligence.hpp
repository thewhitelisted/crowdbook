#pragma once

#include <cstdint>
#include <optional>

#include "crowdbook/agent.hpp"
#include "crowdbook/agents/market_view.hpp"
#include "crowdbook/messages.hpp"
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
    // How the trader's pace follows the market's. Its order rates are multiplied by
    // (recent trade rate / usual trade rate) ^ activityResponse, kept within [0.1, 10], where both
    // rates come from the published trade count it sees when it acts, averaged over
    // activityMemory for recent and over activityBaseline for usual. 0 keeps a steady pace; the
    // nearer 1, the more busy spells breed more activity.
    double activityResponse = 0.0;
    Duration activityMemory = 60 * kSecond;
    Duration activityBaseline = 1'800 * kSecond;
    // How the trader's limit orders follow the market's volatility. The range they go into, 1 to
    // maxOffset ticks inside the opposite quote, is stretched by (recent volatility / usual
    // volatility) ^ volatilityResponse, kept within [0.25, 4], where volatility comes from the
    // moves of the mid it sees between its decisions, averaged over the same two windows as
    // activity. Traders who stand back when prices jump thin the book, and a thinner book makes
    // prices jump further.
    double volatilityResponse = 0.0;
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
    // a maxOffset outside 1 to kMaxPrice, sizes that do not satisfy
    // 1 <= minSize <= maxSize <= kMaxQuantity, a negative activity
    // response, or activity windows that are not positive with memory no longer than baseline.
    ZeroIntelligenceTrader(const ZeroIntelligenceConfig& config, Price referencePrice);

    // It only looks at the market when it acts, so it reads snapshots instead of the stream.
    [[nodiscard]] MarketDataMode marketData() const noexcept override {
        return MarketDataMode::Snapshot;
    }
    void onStart(AgentContext& context) override;
    // Tag 0 is the timer for its next order; any other tag is the client order id of a resting
    // order whose lifetime is over.
    void onWakeup(AgentContext& context, std::uint64_t tag) override;

    // The factor its order rates are multiplied by at the moment: 1 without an activity response.
    [[nodiscard]] double pace() const noexcept { return pace_; }
    // The factor its limit orders' range is stretched by: 1 without a volatility response.
    [[nodiscard]] double stretch() const noexcept { return stretch_; }

private:
    // A running average that weighs observations by how recent they are, with no bias toward its
    // starting point: the sums of weighted values and of weights both start at zero.
    struct Average {
        double weightedSum = 0.0;
        double weight = 0.0;
        void add(double value, double keep) noexcept;
        [[nodiscard]] double value() const noexcept { return weightedSum / weight; }
    };

    // Updates its pace and stretch from what it sees when it acts.
    void observeMarket(const MarketSnapshot& market, Timestamp now);
    [[nodiscard]] double orderRate() const noexcept;
    void scheduleNextOrder(AgentContext& context) const;
    void sendLimit(AgentContext& context) const;
    void sendMarket(AgentContext& context) const;

    ZeroIntelligenceConfig config_;
    MarketView market_;
    Timestamp lastSeenAt_ = -1; // when it last read the trade count, or -1 before it has
    std::uint64_t lastSeenTrades_ = 0;
    Average recent_;
    Average usual_;
    double pace_ = 1.0;
    std::optional<double> lastSeenMid_;
    Average recentVariance_; // squared mid moves per second
    Average usualVariance_;
    double stretch_ = 1.0;
};

} // namespace crowdbook

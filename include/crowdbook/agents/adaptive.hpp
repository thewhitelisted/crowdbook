#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "crowdbook/agent.hpp"
#include "crowdbook/agents/belief.hpp"
#include "crowdbook/agents/market_view.hpp"
#include "crowdbook/fundamental.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

struct AdaptiveConfig {
    Duration interval = kSecond;          // how often it looks at the market and decides
    BeliefConfig belief{.noise = 2.0};    // how it sees the value
    Duration fastHalfLife = 2 * kSecond;  // the trend strategy's moving averages of the price
    Duration slowHalfLife = 20 * kSecond;
    Duration memory = 60 * kSecond;       // half-life of each strategy's track record
    double choiceIntensity = 1.0;         // per tick of record: how surely the better one wins
    double threshold = 1.0;               // ticks of signal before a strategy calls a trade
    Quantity orderSize = 2;
    Quantity maxPosition = 20;
};

// A trader that switches between two strategies by how well each has been doing, after Brock and
// Hommes (1998). Every interval, starting at a random point in the first one, it looks at the price
// and, as its belief sees it, at the true value. The value strategy calls a buy when the value is above the
// price, by at least `threshold`, and a sell when it is below by as much; the trend strategy calls
// a buy when a fast moving average of the price leads a slow one, by at least `threshold`, and a
// sell when it trails by as much. Each strategy keeps a track record: the price change after each
// of its calls, in the call's direction, summed with exponential decay. The trader follows the
// trend strategy with probability 1 / (1 + exp(-choiceIntensity * (trend record - value record))),
// and otherwise the value strategy. Its position follows the chosen strategy's call: long
// maxPosition on a buy, short maxPosition on a sell and flat without a call, which it trades toward
// at the market, at most orderSize at a time, counting orders in flight. Traders like this one
// herd: they all see the same price, so their records agree and they tend to switch together, and
// when they switch they all trade the same way at once.
class AdaptiveTrader final : public Agent {
public:
    // Throws std::invalid_argument without a value, for a non-positive interval or memory, a
    // belief Belief refuses, a negative choice intensity or threshold, half-lives that do not satisfy
    // 0 < fastHalfLife < slowHalfLife, or sizes that do not satisfy
    // 1 <= orderSize <= maxPosition <= kMaxQuantity.
    AdaptiveTrader(const AdaptiveConfig& config, std::shared_ptr<Fundamental> fundamental,
                   Price referencePrice);

    // It looks at the market only when it decides, so it reads snapshots.
    [[nodiscard]] MarketDataMode marketData() const noexcept override {
        return MarketDataMode::Snapshot;
    }
    void onStart(AgentContext& context) override;
    void onWakeup(AgentContext& context, std::uint64_t tag) override;

    // Whether it followed the trend strategy at its latest decision.
    [[nodiscard]] bool followingTrend() const noexcept { return followingTrend_; }
    [[nodiscard]] double trendRecord() const noexcept { return trendRecord_; }
    [[nodiscard]] double valueRecord() const noexcept { return valueRecord_; }

private:
    [[nodiscard]] int call(double signal) const noexcept;

    AdaptiveConfig config_;
    Belief belief_;
    MarketView market_;
    double fastWeight_ = 0.0;
    double slowWeight_ = 0.0;
    double recordDecay_ = 0.0;
    std::optional<double> fast_;
    std::optional<double> slow_;
    std::optional<double> lastPrice_;
    int lastValueCall_ = 0;
    int lastTrendCall_ = 0;
    double trendRecord_ = 0.0;
    double valueRecord_ = 0.0;
    bool followingTrend_ = false;
};

} // namespace crowdbook

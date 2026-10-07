#pragma once

#include <cstdint>
#include <optional>

#include "crowdbook/agent.hpp"
#include "crowdbook/agents/market_view.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

struct MomentumConfig {
    Duration interval = 50 * kMillisecond; // how often it checks the trend
    Duration fastHalfLife = 200 * kMillisecond;
    Duration slowHalfLife = 2 * kSecond;
    double threshold = 2.0; // ticks the fast average must lead the slow one by
    Quantity orderSize = 5;
    Quantity maxPosition = 50;
};

// A trend follower. Every interval it updates a fast and a slow exponential moving average of the
// fair price it sees (the mid, else the last trade). When the fast average is more than
// `threshold` ticks above the slow one it buys at the market, and when it is that far below it
// sells, keeping its position within ±maxPosition even if every order in flight fills.
class MomentumTrader final : public Agent {
public:
    // Throws std::invalid_argument unless 0 < fastHalfLife < slowHalfLife, the interval is
    // positive, the threshold is not negative and 1 <= orderSize <= maxPosition.
    MomentumTrader(const MomentumConfig& config, Price referencePrice);

    // It samples the market on its own timer, so it reads snapshots instead of the stream.
    [[nodiscard]] MarketDataMode marketData() const noexcept override {
        return MarketDataMode::Snapshot;
    }
    void onStart(AgentContext& context) override;
    void onWakeup(AgentContext& context, std::uint64_t tag) override;

    // The fast average minus the slow one, in ticks; 0 before the first check.
    [[nodiscard]] double trend() const noexcept;

private:
    MomentumConfig config_;
    MarketView market_;
    double fastWeight_ = 0.0;
    double slowWeight_ = 0.0;
    std::optional<double> fast_;
    std::optional<double> slow_;
};

} // namespace crowdbook

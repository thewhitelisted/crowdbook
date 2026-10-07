#pragma once

#include <cstdint>
#include <memory>

#include "crowdbook/agent.hpp"
#include "crowdbook/agents/market_view.hpp"
#include "crowdbook/fundamental.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

struct InformedConfig {
    Duration interval = 100 * kMillisecond; // how often it looks at the value
    double noise = 1.0;     // ticks: standard deviation of each private observation of the value
    double threshold = 3.0; // ticks of edge required before trading
    Quantity orderSize = 5;
    Quantity maxPosition = 50;
};

// A trader with private information about the asset's value. Every interval it observes the
// fundamental value with its own noise. When the best ask it knows of is at least `threshold`
// below its estimate it buys with an immediate-or-cancel limit order priced `threshold` below the
// estimate, so it never gives its edge away by sweeping the book; symmetrically, it sells to bids
// above the estimate. Its position stays within ±maxPosition even if every order in flight fills.
class InformedTrader final : public Agent {
public:
    // Throws std::invalid_argument without a fundamental, for a non-positive interval, negative
    // noise or threshold, or sizes that do not satisfy 1 <= orderSize <= maxPosition.
    InformedTrader(const InformedConfig& config, std::shared_ptr<Fundamental> fundamental);

    // It looks at the book only when it checks the value, so it reads snapshots.
    [[nodiscard]] MarketDataMode marketData() const noexcept override {
        return MarketDataMode::Snapshot;
    }
    void onStart(AgentContext& context) override;
    void onWakeup(AgentContext& context, std::uint64_t tag) override;

private:
    InformedConfig config_;
    std::shared_ptr<Fundamental> fundamental_;
    MarketView market_{0};
};

} // namespace crowdbook

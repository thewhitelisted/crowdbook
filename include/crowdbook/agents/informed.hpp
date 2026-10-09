#pragma once

#include <cstdint>
#include <memory>

#include "crowdbook/agent.hpp"
#include "crowdbook/agents/belief.hpp"
#include "crowdbook/agents/market_view.hpp"
#include "crowdbook/fundamental.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

struct InformedConfig {
    Duration interval = 100 * kMillisecond; // how often it looks at the value
    BeliefConfig belief{};  // how it sees the value: noise, a lasting error, a bias, a delay
    // Ticks of edge required before trading: half the width of the range of values it thinks
    // possible, inside which it does nothing.
    double threshold = 3.0;
    Quantity orderSize = 5;
    Quantity maxPosition = 50;
};

// A trader with information about the asset's value. Every interval, starting at a random point in
// the first one so that a group of them does not act in lockstep, it looks at the value as its
// belief sees it: late, with errors of its own, perhaps tilted. When the best ask it knows of is at least `threshold`
// below its estimate it buys with an immediate-or-cancel limit order priced `threshold` below the
// estimate, so it never gives its edge away by sweeping the book; symmetrically, it sells to bids
// above the estimate. Its position stays within ±maxPosition even if every order in flight fills.
class InformedTrader final : public Agent {
public:
    // Throws std::invalid_argument without a value, for a non-positive interval, a negative
    // threshold, a belief Belief refuses, or sizes that do not satisfy 1 <= orderSize <=
    // maxPosition <= kMaxQuantity.
    InformedTrader(const InformedConfig& config, std::shared_ptr<Fundamental> fundamental);

    // It looks at the book only when it checks the value, so it reads snapshots.
    [[nodiscard]] MarketDataMode marketData() const noexcept override {
        return MarketDataMode::Snapshot;
    }
    void onStart(AgentContext& context) override;
    void onWakeup(AgentContext& context, std::uint64_t tag) override;

private:
    InformedConfig config_;
    Belief belief_;
    MarketView market_{0};
};

} // namespace crowdbook

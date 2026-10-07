// A user-written agent trading in a scenario next to the built-in ones. After building with the
// dev preset, run ./build/dev/examples/custom_agent.

#include <iostream>
#include <memory>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/scenario.hpp"
#include "crowdbook/scenario_file.hpp"

namespace {

// Bets on prices reverting: buys one lot when a trade prints more than `band` ticks below the
// recent average trade price, sells one when a trade prints that far above it, and never holds
// more than 20 lots either way, counting orders still in flight.
class MeanReverter final : public crowdbook::Agent {
public:
    MeanReverter(double band, crowdbook::Price referencePrice)
        : band_(band), average_(static_cast<double>(referencePrice)) {}

    void onTrade(crowdbook::AgentContext& context, const crowdbook::Trade& trade) override {
        const auto price = static_cast<double>(trade.price);
        const crowdbook::Ledger& ledger = context.ledger();
        const crowdbook::Quantity mostLong =
            ledger.position() + ledger.openQuantity(crowdbook::Side::Buy);
        const crowdbook::Quantity mostShort =
            ledger.position() - ledger.openQuantity(crowdbook::Side::Sell);
        if (price < average_ - band_ && mostLong < kMaxPosition) {
            context.submitMarket(crowdbook::Side::Buy, 1);
        } else if (price > average_ + band_ && mostShort > -kMaxPosition) {
            context.submitMarket(crowdbook::Side::Sell, 1);
        }
        average_ += 0.05 * (price - average_);
    }

private:
    static constexpr crowdbook::Quantity kMaxPosition = 20;
    double band_;
    double average_;
};

constexpr const char* kScenario = R"(
seed = 3
duration = "20s"

[[agents]]
type = "zero_intelligence"
name = "noise"
count = 20

[[agents]]
type = "mean_reverter"   # registered in main below
name = "reverter"
band = 4.0
)";

} // namespace

int main() {
    crowdbook::AgentRegistry registry = crowdbook::AgentRegistry::withBuiltIns();
    registry.add("mean_reverter", [](const crowdbook::Parameters& parameters,
                                     const crowdbook::Environment& environment) {
        return std::make_unique<MeanReverter>(parameters.number("band", 3.0),
                                              environment.referencePrice);
    });

    const crowdbook::RunResult result =
        crowdbook::runScenario(crowdbook::parseScenario(kScenario), registry);
    std::cout << result.trades << " trades, last price " << result.lastPrice << '\n';
    for (const crowdbook::GroupResult& group : result.groups) {
        std::cout << group.name << ": position " << group.position << ", pnl " << group.pnl
                  << '\n';
    }
}

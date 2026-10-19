#include <cmath>
#include <cstdint>
#include <format>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/event_log.hpp"
#include "crowdbook/scenario.hpp"
#include "crowdbook/scenario_file.hpp"

// A maker that reads order flow sets the price: with the value standing still, its mid moves only
// as the flow moves its fair price. A permanent impact makes the mid a random walk of the flow;
// one that fades makes it revert, so that its changes over a minute are much less than sixty
// times its changes over a second. This is the known answer the fits of books that revert rely
// on (docs/design.md, "A maker that sets the price").

namespace crowdbook {
namespace {

std::string market(std::string_view flow) {
    return std::format(R"(
duration = "600s"
reference_price = 10000

[fundamental]
volatility = 0.0

[exchange]
depth_levels = 10

[[agents]]
type = "market_maker"
quote_value = true
noise = 0.0
risk_aversion = 0.00001
volatility = 0.5
intensity = 1.0
quote_size = 50
levels = 5
max_inventory = 100000
requote_interval = "200ms"
post_only = true
{}
account = {{ max_position = 200000, max_order_quantity = 100000 }}

[[agents]]
type = "zero_intelligence"
count = 10
limit_rate = 0.0
market_rate = 0.5
max_offset = 3
min_size = 1
max_size = 20
account = {{ max_position = 100000, max_order_quantity = 100 }}
)",
                       flow);
}

// The variance of the mid's 60 s changes over 60 times that of its 1 s changes, pooled over
// seeds: 1 for a random walk, less for a mid that reverts.
double varianceRatio(std::string_view flow) {
    double one = 0.0;
    double sixty = 0.0;
    std::int64_t ones = 0;
    std::int64_t sixties = 0;
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        Scenario scenario = parseScenario(market(flow), "flow");
        scenario.seed = seed;
        std::ostringstream prices;
        PriceSampler sampler{prices, kSecond};
        static_cast<void>(runScenario(scenario, AgentRegistry::withBuiltIns(), &sampler));
        sampler.finish(scenario.duration);
        std::vector<double> mids;
        std::istringstream rows{prices.str()};
        std::string row;
        std::getline(rows, row); // the header
        while (std::getline(rows, row)) {
            std::istringstream fields{row};
            std::string time;
            std::string bid;
            std::string ask;
            std::getline(fields, time, ',');
            std::getline(fields, bid, ',');
            std::getline(fields, ask, ',');
            mids.push_back(bid.empty() || ask.empty()
                               ? std::nan("")
                               : (std::stod(bid) + std::stod(ask)) / 2.0);
        }
        for (std::size_t i = 1; i < mids.size(); ++i) {
            if (std::isfinite(mids[i] - mids[i - 1])) {
                one += (mids[i] - mids[i - 1]) * (mids[i] - mids[i - 1]);
                ++ones;
            }
            if (i >= 60 && std::isfinite(mids[i] - mids[i - 60])) {
                sixty += (mids[i] - mids[i - 60]) * (mids[i] - mids[i - 60]);
                ++sixties;
            }
        }
    }
    EXPECT_GT(one, 0.0) << "the mid never moved";
    return (sixty / static_cast<double>(sixties)) / (60.0 * one / static_cast<double>(ones));
}

TEST(FlowReversionTest, AFadingImpactMakesTheMidRevertAndAPermanentOneDoesNot) {
    const double permanent = varianceRatio("flow_impact = 0.05");
    const double fading = varianceRatio("flow_impact = 0.05\nflow_memory = \"20s\"");
    EXPECT_GT(permanent, 0.6) << permanent;  // about 1: a walk of the flow
    EXPECT_LT(fading, 0.5) << fading;        // about 0.35: it reverts within the memory
}

} // namespace
} // namespace crowdbook

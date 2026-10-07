#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "crowdbook/scenario.hpp"

namespace crowdbook {
namespace {

AgentGroup group(std::string type, std::int64_t count, std::string name = "") {
    return {.name = std::move(name),
            .type = std::move(type),
            .count = count,
            .options = {.account = {.maxPosition = 300, .maxOrderQuantity = 20}}};
}

Scenario marketWithAMaker() {
    Scenario scenario{.seed = 5, .duration = 5 * kSecond, .referencePrice = 1'000};
    scenario.groups.push_back(group("market_maker", 1));
    scenario.groups.push_back(group("zero_intelligence", 15, "noise"));
    scenario.groups.push_back(group("momentum", 3));
    return scenario;
}

TEST(ScenarioTest, RunsAMarketWhereMoneyIsConserved) {
    const RunResult result = runScenario(marketWithAMaker(), AgentRegistry::withBuiltIns());

    EXPECT_GT(result.trades, 100U);
    EXPECT_GT(result.volume, 0);
    ASSERT_EQ(result.groups.size(), 3U);
    EXPECT_EQ(result.groups[0].name, "market_maker");
    EXPECT_EQ(result.groups[1].name, "noise");
    EXPECT_EQ(result.groups[1].agents.size(), 15U);

    // Trading only moves cash and shares between agents, so the groups' totals cancel out.
    Cash cash = 0;
    Quantity position = 0;
    Cash pnl = 0;
    for (const GroupResult& summary : result.groups) {
        cash += summary.cash;
        position += summary.position;
        pnl += summary.pnl;
    }
    EXPECT_EQ(cash, 0);
    EXPECT_EQ(position, 0);
    EXPECT_EQ(pnl, 0);
}

TEST(ScenarioTest, SameScenarioGivesTheSameResult) {
    const AgentRegistry registry = AgentRegistry::withBuiltIns();
    const RunResult first = runScenario(marketWithAMaker(), registry);
    const RunResult second = runScenario(marketWithAMaker(), registry);
    EXPECT_EQ(first.trades, second.trades);
    EXPECT_EQ(first.lastPrice, second.lastPrice);
    for (std::size_t i = 0; i < first.groups.size(); ++i) {
        EXPECT_EQ(first.groups[i].pnl, second.groups[i].pnl);
    }
}

TEST(ScenarioTest, InformedTradersSeeTheFundamental) {
    Scenario scenario{.seed = 2, .duration = 5 * kSecond, .referencePrice = 1'000};
    scenario.fundamental = FundamentalConfig{.initial = 1'000.0, .volatility = 5.0};
    scenario.groups.push_back(group("zero_intelligence", 15));
    scenario.groups.push_back(group("informed", 3));

    const RunResult result = runScenario(scenario, AgentRegistry::withBuiltIns());
    EXPECT_NE(result.groups[1].position, 0); // the informed traders did trade
    ASSERT_TRUE(result.finalValue.has_value());
    EXPECT_NE(*result.finalValue, 1'000.0); // the value moved during the run
    EXPECT_FALSE(runScenario(marketWithAMaker(), AgentRegistry::withBuiltIns()).finalValue);
}

TEST(ScenarioTest, NamesTheGroupAtFault) {
    Scenario scenario = marketWithAMaker();
    scenario.groups[1].parameters.set("limt_rate", 2.0);
    try {
        static_cast<void>(runScenario(scenario, AgentRegistry::withBuiltIns()));
        FAIL() << "expected a ScenarioError";
    } catch (const ScenarioError& error) {
        EXPECT_EQ(std::string{error.what()},
                  "agent group 2 (noise): unknown parameter for agent type "
                  "'zero_intelligence': limt_rate");
    }

    Scenario noFundamental = marketWithAMaker();
    noFundamental.groups.push_back(group("informed", 1));
    EXPECT_THROW(static_cast<void>(runScenario(noFundamental, AgentRegistry::withBuiltIns())),
                 ScenarioError);

    Scenario empty;
    EXPECT_THROW(static_cast<void>(runScenario(empty, AgentRegistry::withBuiltIns())),
                 ScenarioError);
}

} // namespace
} // namespace crowdbook

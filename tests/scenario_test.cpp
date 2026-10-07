#include <cstddef>
#include <cstdint>
#include <sstream>
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

TEST(ScenarioTest, CountsEachGroupsVolumeAndStartingBalances) {
    Scenario scenario = marketWithAMaker();
    scenario.groups[1].options.account.initialCash = 1'000;
    scenario.groups[1].options.account.initialPosition = 4;
    const RunResult result = runScenario(scenario, AgentRegistry::withBuiltIns());

    // Every lot traded has a buyer and a seller.
    Quantity traded = 0;
    for (const GroupResult& summary : result.groups) {
        traded += summary.traded;
    }
    EXPECT_EQ(traded, 2 * result.volume);
    EXPECT_GT(result.groups[0].traded, 0);
    EXPECT_EQ(result.groups[1].initialCash, 15 * 1'000);
    EXPECT_EQ(result.groups[1].initialPosition, 15 * 4);
    EXPECT_EQ(result.groups[1].pnl,
              (result.groups[1].cash - 15'000) +
                  (result.groups[1].position - 60) * result.lastPrice);
}

TEST(ScenarioTest, WritesResultsAsJson) {
    Scenario scenario{.seed = 3, .duration = 2 * kSecond, .referencePrice = 500};
    RunResult result{.trades = 4, .volume = 9, .lastPrice = 501, .finalValue = 499.5};
    result.groups.push_back(GroupResult{.name = "a \"quoted\" name",
                                        .type = "market_maker",
                                        .agents = {1, 2},
                                        .traded = 9,
                                        .initialCash = 10,
                                        .initialPosition = -1,
                                        .cash = 20,
                                        .position = 3,
                                        .pnl = 2014});
    std::ostringstream out;
    writeResultJson(out, scenario, result);
    EXPECT_EQ(out.str(), "{\n"
                         "  \"seed\": 3,\n"
                         "  \"duration_ns\": 2000000000,\n"
                         "  \"reference_price\": 500,\n"
                         "  \"trades\": 4,\n"
                         "  \"volume\": 9,\n"
                         "  \"last_price\": 501,\n"
                         "  \"final_value\": 499.5,\n"
                         "  \"groups\": [\n"
                         "    {\"name\": \"a \\\"quoted\\\" name\", \"type\": \"market_maker\", "
                         "\"agents\": 2, \"traded\": 9, \"initial_cash\": 10, "
                         "\"initial_position\": -1, \"cash\": 20, \"position\": 3, \"pnl\": 2014}\n"
                         "  ]\n"
                         "}\n");
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

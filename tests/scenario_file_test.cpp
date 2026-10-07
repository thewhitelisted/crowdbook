#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/scenario_file.hpp"

namespace crowdbook {
namespace {

// The message of the ScenarioError that parsing `text` throws, or "" if it parses.
std::string parseError(std::string_view text) {
    try {
        static_cast<void>(parseScenario(text, "test.toml"));
    } catch (const ScenarioError& error) {
        return error.what();
    }
    return "";
}

TEST(ScenarioFileTest, ReadsEverySetting) {
    const Scenario scenario = parseScenario(R"(
seed = 42
duration = "90s"
reference_price = 5000

[fundamental]
mean_reversion = 0.5
volatility = 3.0
step = "50ms"
jump_rate = 0.01
jump_size = 8.0

[exchange]
depth_levels = 5
maker_fee = -0.25
taker_fee = 0.3

[participant]
latency = { to_exchange = "1ms", from_exchange = "2ms" }
account = { max_position = 100, max_order_quantity = 20 }

[[agents]]
type = "market_maker"
name = "maker"
start = "1s"
latency = { to_exchange = "20us", from_exchange = "30us", jitter = "5us" }
quote_size = 10
volatility = 2.5
quiet = true
style = "tight"

[agents.account]
initial_cash = 1000
initial_position = -5
max_position = 200
max_order_quantity = 25

[[agents]]
type = "zero_intelligence"
count = 30
)");

    EXPECT_EQ(scenario.seed, 42U);
    EXPECT_EQ(scenario.duration, 90 * kSecond);
    EXPECT_EQ(scenario.referencePrice, 5'000);
    ASSERT_TRUE(scenario.fundamental.has_value());
    EXPECT_EQ(scenario.fundamental->initial, 5'000.0); // defaults to the reference price
    EXPECT_EQ(scenario.fundamental->meanReversion, 0.5);
    EXPECT_EQ(scenario.fundamental->volatility, 3.0);
    EXPECT_EQ(scenario.fundamental->step, 50 * kMillisecond);
    EXPECT_EQ(scenario.fundamental->jumpRate, 0.01);
    EXPECT_EQ(scenario.fundamental->jumpSize, 8.0);
    EXPECT_EQ(scenario.exchange.depthLevels, 5U);
    EXPECT_EQ(scenario.exchange.makerFee, -250);
    EXPECT_EQ(scenario.exchange.takerFee, 300);
    EXPECT_EQ(scenario.participant.latency.toExchange, kMillisecond);
    EXPECT_EQ(scenario.participant.latency.fromExchange, 2 * kMillisecond);
    EXPECT_EQ(scenario.participant.account.maxPosition, 100);
    EXPECT_EQ(scenario.participant.account.maxOrderQuantity, 20);

    ASSERT_EQ(scenario.groups.size(), 2U);
    const AgentGroup& maker = scenario.groups[0];
    EXPECT_EQ(maker.type, "market_maker");
    EXPECT_EQ(maker.name, "maker");
    EXPECT_EQ(maker.count, 1);
    EXPECT_EQ(maker.options.startTime, kSecond);
    EXPECT_EQ(maker.options.latency.toExchange, 20 * kMicrosecond);
    EXPECT_EQ(maker.options.latency.fromExchange, 30 * kMicrosecond);
    EXPECT_EQ(maker.options.latency.jitter, 5 * kMicrosecond);
    EXPECT_EQ(maker.options.account.initialCash, 1'000);
    EXPECT_EQ(maker.options.account.initialPosition, -5);
    EXPECT_EQ(maker.options.account.maxPosition, 200);
    EXPECT_EQ(maker.options.account.maxOrderQuantity, 25);
    EXPECT_EQ(maker.parameters.unusedNames(),
              (std::vector<std::string>{"quiet", "quote_size", "style", "volatility"}));
    EXPECT_EQ(maker.parameters.integer("quote_size", 0), 10);
    EXPECT_EQ(maker.parameters.number("volatility", 0.0), 2.5);
    EXPECT_TRUE(maker.parameters.flag("quiet", false));
    EXPECT_EQ(maker.parameters.text("style", ""), "tight");

    EXPECT_EQ(scenario.groups[1].type, "zero_intelligence");
    EXPECT_EQ(scenario.groups[1].count, 30);
}

TEST(ScenarioFileTest, UsesDefaultsForWhatIsLeftOut) {
    const Scenario scenario = parseScenario("[[agents]]\ntype = \"zero_intelligence\"\n");
    EXPECT_EQ(scenario.seed, 1U);
    EXPECT_EQ(scenario.duration, 60 * kSecond);
    EXPECT_EQ(scenario.referencePrice, 10'000);
    EXPECT_FALSE(scenario.fundamental.has_value());
    ASSERT_EQ(scenario.groups.size(), 1U);
    EXPECT_EQ(scenario.groups[0].count, 1);
    EXPECT_EQ(scenario.groups[0].options.startTime, std::nullopt);
}

TEST(ScenarioFileTest, ReportsSyntaxErrorsWithTheirLine) {
    EXPECT_TRUE(parseError("seed = 1\nduration = \n").starts_with("test.toml:2:"));
}

TEST(ScenarioFileTest, RejectsUnknownKeysWithTheirLine) {
    EXPECT_EQ(parseError("seed = 1\nduraton = \"5s\"\n[[agents]]\ntype = \"momentum\"\n"),
              "test.toml:2: unknown key 'duraton' at the top level; expected one of: seed, "
              "duration, reference_price, fundamental, exchange, agents, participant");
    EXPECT_EQ(parseError("[[agents]]\ntype = \"momentum\"\n"
                         "latency = { to_exchange = \"1us\", jiter = \"1us\" }\n"),
              "test.toml:3: unknown key 'jiter' in latency; expected one of: to_exchange, "
              "from_exchange, jitter");
}

TEST(ScenarioFileTest, RejectsValuesOfTheWrongKindOrRange) {
    const std::string agent = "[[agents]]\ntype = \"momentum\"\n";
    const std::vector<std::pair<std::string, std::string>> cases = {
        {agent + "count = 0\n", "'count' must be between 1 and 1000000"},
        {"duration = 5\n" + agent, "'duration' must be a duration in quotes"},
        {"duration = \"5 minutes\"\n" + agent, "'5 minutes' is not a valid duration"},
        {"reference_price = 0\n" + agent, "'reference_price' must be between 1 and 1000000000"},
        {"agents = 3\n", "agents must be written as [[agents]] tables"},
        {"[[agents]]\ncount = 2\n", "every [[agents]] table needs a type"},
        {agent + "weights = { a = 1 }\n", "agent parameter 'weights' must be a number"},
        {agent + "account = { max_position = -1 }\n", "'max_position' must be between 0"},
        {"seed = 1\n", "the scenario has no [[agents]]"},
        {"[exchange]\ntaker_fee = 0.0005\n" + agent, "at most three decimals"},
        {"[exchange]\nmaker_fee = -0.5\ntaker_fee = 0.3\n" + agent,
         "maker_fee plus taker_fee must not be negative"},
        {"[exchange]\nfee = 1\n" + agent, "unknown key 'fee' in [exchange]"},
        {"[exchange]\ndepth_levels = -1\n" + agent, "'depth_levels' must be between 0 and 1000"},
        {"[exchange]\nmaker_fee = nan\n" + agent, "'maker_fee' must be a finite number"},
        {"[fundamental]\nvolatility = inf\n" + agent, "'volatility' must be a finite number"},
        {"[fundamental]\ninitial = -nan\n" + agent, "'initial' must be a finite number"},
        {agent + "rate = -inf\n", "agent parameter 'rate' must be a finite number"},
        {"duration = \"1000000000s\"\n" + agent, "it must be shorter than 1000000000s"},
    };
    for (const auto& [text, expected] : cases) {
        EXPECT_NE(parseError(text).find(expected), std::string::npos)
            << "for:\n" << text << "got: " << parseError(text);
    }
}

TEST(ScenarioFileTest, LoadsFilesAndReportsMissingOnes) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "crowdbook_scenario_file_test.toml";
    {
        std::ofstream file{path};
        file << "seed = 9\n[[agents]]\ntype = \"zero_intelligence\"\n";
    }
    EXPECT_EQ(loadScenario(path).seed, 9U);
    std::filesystem::remove(path);
    EXPECT_THROW(static_cast<void>(loadScenario(path)), ScenarioError);
}

} // namespace
} // namespace crowdbook

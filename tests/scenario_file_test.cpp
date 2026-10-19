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

TEST(ScenarioFileTest, ReadsLatenciesDrawnFromQuantiles) {
    const Scenario scenario = parseScenario(R"(
[participant]
latency = { to_exchange = { quantiles = ["8ms", "9ms", "12ms", "40ms"] }, from_exchange = "3ms" }

[[agents]]
type = "momentum"
latency = { from_exchange = { quantiles = ["1ms", "2ms", "30ms"] }, jitter = "1us" }
)");
    const Latency& seat = scenario.participant.latency;
    EXPECT_EQ(seat.toExchangeQuantiles,
              (std::vector<Duration>{8 * kMillisecond, 9 * kMillisecond, 12 * kMillisecond,
                                     40 * kMillisecond}));
    EXPECT_EQ(seat.toExchange, 10'500 * kMicrosecond); // the median
    EXPECT_TRUE(seat.fromExchangeQuantiles.empty());
    EXPECT_EQ(seat.fromExchange, 3 * kMillisecond);
    const Latency& agent = scenario.groups[0].options.latency;
    EXPECT_EQ(agent.fromExchangeQuantiles,
              (std::vector<Duration>{kMillisecond, 2 * kMillisecond, 30 * kMillisecond}));
    EXPECT_EQ(agent.fromExchange, 2 * kMillisecond);
    EXPECT_EQ(agent.longestFromExchange(), 30 * kMillisecond + kMicrosecond);
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
              "test.toml:2: unknown key 'duraton' at the top level; expected one of: "
              "scenario_version, seed, duration, reference_price, fundamental, prediction, "
              "exchange, trading_day, agents, participant, scoring, challenge, learning");
    EXPECT_EQ(parseError("[[agents]]\ntype = \"momentum\"\n"
                         "latency = { to_exchange = \"1us\", jiter = \"1us\" }\n"),
              "test.toml:3: unknown key 'jiter' in latency; expected one of: to_exchange, "
              "from_exchange, jitter");
}

TEST(ScenarioFileTest, RejectsValuesOfTheWrongKindOrRange) {
    const std::string agent = "[[agents]]\ntype = \"momentum\"\n";
    const std::vector<std::pair<std::string, std::string>> cases = {
        {agent + "count = -1\n", "'count' must be between 0 and 1000000"},
        {"duration = 5\n" + agent, "'duration' must be a duration in quotes"},
        {"duration = \"5 minutes\"\n" + agent, "'5 minutes' is not a valid duration"},
        {"reference_price = 0\n" + agent, "'reference_price' must be between 1 and 1000000000"},
        {"agents = 3\n", "agents must be written as [[agents]] tables"},
        {"[[agents]]\ncount = 2\n", "every [[agents]] table needs a type"},
        {agent + "weights = { a = 1 }\n", "agent parameter 'weights' must be a number"},
        {agent + "account = { max_position = -1 }\n", "'max_position' must be between 0"},
        {agent + "latency = { to_exchange = { quantiles = [\"2ms\", \"1ms\"] } }\n",
         "'to_exchange' quantiles must be in order, shortest first"},
        {agent + "latency = { from_exchange = { quantiles = [] } }\n",
         "'from_exchange' quantiles must be a list of one or more durations"},
        {agent + "latency = { to_exchange = { median = \"1ms\" } }\n",
         "unknown key 'median' in 'to_exchange'; expected one of: quantiles"},
        {agent + "latency = { to_exchange = { quantiles = [1, 2] } }\n",
         "'to_exchange' must be a duration in quotes"},
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
        {"[scoring]\nmark = \"value\"\n" + agent,
         "scoring at the true value needs a [fundamental] or [prediction] section"},
        {"[scoring]\nmark = \"mid\"\n" + agent,
         "'mark' must be \"last\", \"value\" or \"book\""},
        {"[scoring]\ninventory_penalty = -0.1\n" + agent, "not negative"},
        {"[scoring]\nclose_penalty = 0.0001\n" + agent, "at most three decimals"},
        {"[scoring]\nmax_loss = -5\n" + agent, "'max_loss' must be between 0"},
        {"[scoring]\npenalty = 1\n" + agent, "unknown key 'penalty' in [scoring]"},
        {"[scoring.target]\nside = \"buy\"\n" + agent, "missing 'quantity'"},
        {"[scoring.target]\nside = \"long\"\nquantity = 5\n" + agent,
         "'side' must be \"buy\" or \"sell\""},
        {"[scoring.target]\nside = \"buy\"\nquantity = 5\nbenchmark = \"twap\"\n" + agent,
         "'benchmark' must be \"vwap\" or \"reference\""},
        {"[challenge]\nbriefing = \"go\"\n" + agent, "missing 'name'"},
        {"scenario_version = 2\n" + agent,
         "this scenario is for a newer crowdbook: it has scenario_version 2, and this one reads "
         "up to 1"},
        {"scenario_version = 0\n" + agent, "'scenario_version' must be a whole number from 1"},
        {"[challenge]\nname = \"\"\nbriefing = \"go\"\n" + agent, "a challenge needs a name"},
    };
    for (const auto& [text, expected] : cases) {
        EXPECT_NE(parseError(text).find(expected), std::string::npos)
            << "for:\n" << text << "got: " << parseError(text);
    }
}

TEST(ScenarioFileTest, ReadsScoringAndChallenges) {
    const Scenario scenario = parseScenario(R"(
[challenge]
name = "Work a large order"
briefing = """
Buy it all."""

[scoring]
inventory_penalty = 0.02
close_penalty = 1
max_loss = 2000

[scoring.target]
side = "sell"
quantity = 300
benchmark = "reference"
unfinished_penalty = 10.5

[[agents]]
type = "momentum"
)");
    ASSERT_TRUE(scenario.challenge);
    EXPECT_EQ(scenario.challenge->name, "Work a large order");
    EXPECT_EQ(scenario.challenge->briefing, "Buy it all.");
    ASSERT_TRUE(scenario.scoring);
    EXPECT_EQ(scenario.scoring->mark, Mark::LastTrade);
    EXPECT_EQ(scenario.scoring->inventoryPenalty, 20);
    EXPECT_EQ(scenario.scoring->closePenalty, 1'000);
    EXPECT_EQ(scenario.scoring->maxLoss, 2'000);
    ASSERT_TRUE(scenario.scoring->target);
    EXPECT_EQ(scenario.scoring->target->side, Side::Sell);
    EXPECT_EQ(scenario.scoring->target->quantity, 300);
    EXPECT_EQ(scenario.scoring->target->benchmark, Benchmark::Reference);
    EXPECT_EQ(scenario.scoring->target->unfinishedPenalty, 10'500);

    EXPECT_EQ(parseScenario("[scoring]\nmark = \"book\"\n[[agents]]\ntype = \"momentum\"\n")
                  .scoring->mark,
              Mark::Book);
    EXPECT_FALSE(parseScenario("[[agents]]\ntype = \"momentum\"\n").scoring);
    EXPECT_NO_THROW(static_cast<void>(
        parseScenario("scenario_version = 1\n[[agents]]\ntype = \"momentum\"\n")));
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

TEST(ScenarioFileTest, ReadsAPredictionMarket) {
    const Scenario scenario = parseScenario(R"(duration = "600s"
[prediction]
probability = 0.37
news_rate = 0.02
news_share = 0.6
step = "500ms"
[[agents]]
type = "zero_intelligence"
)");
    ASSERT_TRUE(scenario.prediction);
    EXPECT_EQ(*scenario.prediction, (PredictionConfig{.probability = 0.37,
                                                      .newsRate = 0.02,
                                                      .newsShare = 0.6,
                                                      .step = 500 * kMillisecond}));
    // Agents anchor at the starting probability, and the exchange takes prices up to 99.
    EXPECT_EQ(scenario.referencePrice, 37);
    EXPECT_EQ(exchangeConfig(scenario).maxPrice, 99);
    const auto value = makeFundamental(scenario);
    ASSERT_TRUE(value);
    EXPECT_EQ(value->resolution(), 600 * kSecond);
    EXPECT_NEAR(value->valueAt(0), 37.0, 1e-9);

    const Scenario defaults = parseScenario("[prediction]\n[[agents]]\ntype = \"momentum\"\n");
    EXPECT_EQ(*defaults.prediction, PredictionConfig{});
    EXPECT_EQ(defaults.referencePrice, 50);
    EXPECT_FALSE(makeFundamental(parseScenario("[[agents]]\ntype = \"momentum\"\n")));
}

TEST(ScenarioFileTest, ReadsARecordedValuePath) {
    const Scenario scenario = parseScenario(R"(duration = "120s"
[prediction]
probability = 0.52
path = [[0, 52], [30.5, 54.25], [90, 71]]
outcome = 100
[[agents]]
type = "zero_intelligence"
)");
    ASSERT_TRUE(scenario.prediction);
    EXPECT_EQ(scenario.prediction->path,
              (std::vector<ValuePoint>{{.at = 0, .value = 52.0},
                                       {.at = 30'500 * kMillisecond, .value = 54.25},
                                       {.at = 90 * kSecond, .value = 71.0}}));
    EXPECT_EQ(scenario.prediction->outcome, 100.0);
    const auto value = makeFundamental(scenario);
    ASSERT_TRUE(value);
    EXPECT_NEAR(value->valueAt(31 * kSecond), 54.25, 1e-12);
    EXPECT_NEAR(value->valueAt(120 * kSecond), 100.0, 1e-12);

    const std::string agent = "[[agents]]\ntype = \"momentum\"\n";
    for (const auto& [text, message] : std::vector<std::pair<std::string, std::string>>{
             {"[prediction]\npath = 3\n" + agent, "'path' must be a list"},
             {"[prediction]\npath = []\n" + agent, "'path' is empty"},
             {"[prediction]\npath = [[1e10, 50]]\n" + agent, "within 1000000000s"},
             {"[prediction]\npath = [[1, 2, 3]]\n" + agent, "a pair: [seconds, cents]"},
             {"[prediction]\npath = [[-1, 50]]\n" + agent, "must not come before the start"},
             {"[prediction]\npath = [[1, 150]]\n" + agent, "from 0 to 100 cents"},
             {"[prediction]\npath = [[2, 50], [1, 50]]\n" + agent, "in order of time"},
             {"[prediction]\noutcome = 101\n" + agent, "'outcome' must be from 0 to 100"},
             {"[prediction]\nnews_rate = 0.1\npath = [[1, 50]]\n" + agent,
              "takes no game and no news"}}) {
        EXPECT_NE(parseError(text).find(message), std::string::npos) << parseError(text);
    }
}

// Any market's value can follow a recording too, in ticks: an index's price, say.
TEST(ScenarioFileTest, ReadsARecordedFundamentalPath) {
    const Scenario scenario = parseScenario(R"(duration = "120s"
[fundamental]
initial = 60125
path = [[0, 60125.5], [30.5, 60131], [90, 60118]]
[[agents]]
type = "zero_intelligence"
)");
    EXPECT_EQ(scenario.fundamental->path,
              (std::vector<ValuePoint>{{.at = 0, .value = 60'125.5},
                                       {.at = 30'500 * kMillisecond, .value = 60'131.0},
                                       {.at = 90 * kSecond, .value = 60'118.0}}));
    EXPECT_EQ(scenario.fundamental->volatility, 0.0);
    const auto value = makeFundamental(scenario);
    ASSERT_TRUE(value);
    EXPECT_NEAR(value->valueAt(31 * kSecond), 60'131.0, 1e-9);
    EXPECT_NEAR(value->valueAt(120 * kSecond), 60'118.0, 1e-9); // no settlement: it holds

    const std::string agent = "[[agents]]\ntype = \"momentum\"\n";
    for (const auto& [text, message] : std::vector<std::pair<std::string, std::string>>{
             {"[fundamental]\npath = []\n" + agent, "'path' must be a list"},
             {"[fundamental]\npath = [[1, 2, 3]]\n" + agent, "a pair: [seconds, ticks]"},
             {"[fundamental]\npath = [[-1, 50]]\n" + agent, "from the start"},
             {"[fundamental]\npath = [[2, 50], [1, 50]]\n" + agent, "in order of time"},
             {"[fundamental]\nvolatility = 2\npath = [[1, 50]]\n" + agent,
              "a recorded 'path' replaces the walk"}}) {
        EXPECT_NE(parseError(text).find(message), std::string::npos) << parseError(text);
    }
}

TEST(ScenarioFileTest, ReadsAGame) {
    const Scenario scenario = parseScenario(R"(duration = "3h"
[prediction]
probability = 0.6
[prediction.game]
side = "away"
start = "1h"
length = "48m"
periods = 4
break = "5m"
scores = 85.0
late_scoring = 1.5
points = [1, 2, 3]
point_weights = [0.2, 0.55, 0.25]
tie = 50
pregame_drift = 0.2
in_play_drift = 0.1
news = 3
news_size = 0.15
script = [{ at = "70m", side = "away", points = 3 }, { at = "71m", side = "home" }]
[[agents]]
type = "zero_intelligence"
)");
    ASSERT_TRUE(scenario.prediction && scenario.prediction->game);
    EXPECT_EQ(*scenario.prediction->game,
              (GameConfig{.side = GameSide::Away,
                          .start = 3600 * kSecond,
                          .length = 2880 * kSecond,
                          .periods = 4,
                          .breakLength = 300 * kSecond,
                          .scores = 85.0,
                          .lateScoring = 1.5,
                          .points = {1, 2, 3},
                          .pointWeights = {0.2, 0.55, 0.25},
                          .tie = 50.0,
                          .pregameDrift = 0.2,
                          .inPlayDrift = 0.1,
                          .news = 3.0,
                          .newsSize = 0.15,
                          .script = {{.at = 4200 * kSecond, .home = false, .points = 3},
                                     {.at = 4260 * kSecond}}}));
    EXPECT_EQ(scenario.prediction->game->end(), 3600 * kSecond + 2880 * kSecond + 900 * kSecond);
    EXPECT_NEAR(makeFundamental(scenario)->valueAt(0), 60.0, 1e-6);
    const Scenario defaults =
        parseScenario("duration = \"2h\"\n[prediction.game]\n[[agents]]\ntype = \"momentum\"\n");
    EXPECT_EQ(*defaults.prediction->game, GameConfig{});
    const Scenario total =
        parseScenario("duration = \"2h\"\n[prediction.game]\nside = \"over\"\nline = 45.5\n"
                      "[[agents]]\ntype = \"momentum\"\n");
    EXPECT_EQ(total.prediction->game->side, GameSide::Over);
    EXPECT_EQ(total.prediction->game->line, 45.5);
}

TEST(ScenarioFileTest, ReadsAnOrderDelay) {
    const Scenario scenario = parseScenario(R"(duration = "2h"
[exchange]
order_delay = "5s"
order_delay_from = "30m"
order_delay_cancels = true
clear_at = ["30m", "1h"]
[[agents]]
type = "momentum"
)");
    EXPECT_EQ(scenario.orderDelay, (OrderDelay{.from = 1800 * kSecond,
                                               .until = 7200 * kSecond,
                                               .delay = 5 * kSecond,
                                               .cancels = true}));
    EXPECT_EQ(parseScenario("[[agents]]\ntype = \"momentum\"\n").orderDelay, OrderDelay{});
    EXPECT_EQ(scenario.clearAt, (std::vector<Duration>{1800 * kSecond, 3600 * kSecond}));
    EXPECT_NE(parseError("[exchange]\norder_delay_from = \"2s\"\n[[agents]]\ntype = \"momentum\"\n")
                  .find("needs an 'order_delay'"),
              std::string::npos);
    EXPECT_NE(parseError("[exchange]\norder_delay = \"1s\"\norder_delay_from = \"2s\"\n"
                         "order_delay_until = \"1s\"\n"
                         "[[agents]]\ntype = \"momentum\"\n")
                  .find("'order_delay_until' must not come before 'order_delay_from'"),
              std::string::npos);
}

TEST(ScenarioFileTest, ReadsAProbabilityFeeCurveForAPredictionMarket) {
    const Scenario scenario = parseScenario(R"([prediction]
[exchange]
taker_fee = 1.75
fee_curve = "probability"
[[agents]]
type = "momentum"
)");
    EXPECT_EQ(scenario.exchange.takerFee, 1'750);
    EXPECT_EQ(scenario.exchange.feeCurve, FeeCurve::Probability);
    EXPECT_EQ(parseScenario("[[agents]]\ntype = \"momentum\"\n").exchange.feeCurve,
              FeeCurve::Flat);
    const std::string agent = "[[agents]]\ntype = \"momentum\"\n";
    EXPECT_NE(parseError("[exchange]\nfee_curve = \"steep\"\n" + agent)
                  .find("test.toml:2: 'fee_curve' must be \"flat\" or \"probability\""),
              std::string::npos);
    EXPECT_NE(parseError("[exchange]\nfee_curve = \"probability\"\n" + agent)
                  .find("needs a market whose shares pay out"),
              std::string::npos);
}

TEST(ScenarioFileTest, RejectsPredictionMarketsThatMakeNoSense) {
    const std::string agents = "\n[[agents]]\ntype = \"momentum\"\n";
    for (const auto& [text, expected] : {
             std::pair{"[prediction]\nprobability = 1.0", "test.toml:2: 'probability' must be"},
             std::pair{"[prediction]\nnews_share = 0.5", "test.toml:2: 'news_share' needs news"},
             std::pair{"[prediction]\nnews_rate = 1.0\nnews_share = 1.0",
                       "test.toml:3: 'news_share' must be at least 0 and below 1"},
             std::pair{"[prediction]\nstep = \"0s\"", "test.toml:2: 'step' must be positive"},
             std::pair{"reference_price = 100\n[prediction]",
                       "test.toml:1: a prediction market trades from 1 to 99"},
             std::pair{"[fundamental]\n[prediction]", "test.toml:2: a prediction market's true"},
             std::pair{"duration = \"100s\"\n[prediction]\nnews_rate = 10.0\nnews_share = 0.5",
                       "prediction: at most 500 pieces of news"},
             std::pair{"[prediction.game]\nlength = \"0s\"",
                       "test.toml:1: a game's 'length' must be positive"},
             std::pair{"[prediction.game]\nhalves = 2", "test.toml:2: unknown key 'halves'"},
             std::pair{"[prediction.game]\nside = \"left\"",
                       "test.toml:2: 'side' is \"home\", \"away\", \"draw\""},
             std::pair{"[prediction.game]\npoints = [2, 3]\npoint_weights = [1.0]",
                       "test.toml:3: 'point_weights' must be a list as long as 'points'"},
             std::pair{"[prediction.game]\nscript = [{ at = \"1m\", side = \"left\" }]",
                       "test.toml:2: a scripted score's 'side' is \"home\" or \"away\""},
             std::pair{"[prediction.game]\nscript = [{ at = \"2m\", side = \"home\" }, "
                       "{ at = \"1m\", side = \"away\" }]",
                       "scripted scores must come in the order they were scored"},
             std::pair{"[prediction]\nnews_rate = 0.1\n[prediction.game]",
                       "prediction: a game brings its own news"},
         }) {
        EXPECT_NE(parseError(std::string{text} + agents).find(expected), std::string::npos)
            << text << "\n" << parseError(std::string{text} + agents);
    }
}

} // namespace
} // namespace crowdbook

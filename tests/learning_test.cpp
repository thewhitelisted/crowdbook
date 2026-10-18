#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/event_log.hpp"
#include "crowdbook/learning.hpp"
#include "crowdbook/random.hpp"
#include "crowdbook/scenario.hpp"
#include "crowdbook/scenario_file.hpp"
#include "crowdbook/session.hpp"

namespace crowdbook {
namespace {

// Rests two bids and two offers at the start and does nothing more.
class StandingBook final : public Agent {
public:
    void onStart(AgentContext& context) override {
        context.submitLimit(Side::Buy, 99, 4);
        context.submitLimit(Side::Buy, 98, 4);
        context.submitLimit(Side::Sell, 101, 4);
        context.submitLimit(Side::Sell, 102, 4);
    }
};

// Sends one market order at a given time.
class Hitter final : public Agent {
public:
    Hitter(Timestamp at, Side side, Quantity quantity)
        : at_(at), side_(side), quantity_(quantity) {}

    void onStart(AgentContext& context) override { context.wakeAt(at_, 0); }
    void onWakeup(AgentContext& context, std::uint64_t /*tag*/) override {
        context.submitMarket(side_, quantity_);
    }

private:
    Timestamp at_;
    Side side_;
    Quantity quantity_;
};

AgentRegistry testRegistry() {
    AgentRegistry registry = AgentRegistry::withBuiltIns();
    registry.add("standing_book", [](const Parameters& /*parameters*/,
                                     const Environment& /*environment*/) {
        return std::make_unique<StandingBook>();
    });
    registry.add("hitter", [](const Parameters& parameters, const Environment& /*environment*/) {
        return std::make_unique<Hitter>(parameters.duration("at", 0),
                                        parameters.text("side", "sell") == "buy" ? Side::Buy
                                                                                 : Side::Sell,
                                        parameters.integer("quantity", 1));
    });
    return registry;
}

const AgentRegistry& registry() {
    static const AgentRegistry kRegistry = testRegistry();
    return kRegistry;
}

// A book of two bids and two offers that nobody else touches, and one market order at 5.5s.
constexpr std::string_view kScripted = R"(
seed = 3
duration = "20s"
reference_price = 100

[exchange]
depth_levels = 5
taker_fee = 0.5

[participant]
latency = { to_exchange = "1ms", from_exchange = "1ms" }
account = { max_position = 10, max_order_quantity = 5 }

[learning]
sizes = [1, 3, 8]
depth_levels = 5

[[agents]]
type = "standing_book"
latency = { to_exchange = "100us", from_exchange = "100us" }

[[agents]]
type = "hitter"
at = "5500ms"
side = "sell"
quantity = 2
)";

// A busy market with a true value, and a feed of it.
constexpr std::string_view kBusy = R"(
seed = 11
duration = "60s"
reference_price = 1000

[fundamental]
volatility = 0.5
step = "250ms"
jump_rate = 0.02
jump_size = 3.0

[exchange]
depth_levels = 5
maker_fee = -0.1
taker_fee = 0.3

[participant]
latency = { to_exchange = "1ms", from_exchange = "1ms" }
account = { max_position = 20, max_order_quantity = 5 }

[learning]
sizes = [1, 3]
depth_levels = 5
max_events = 1024
max_depth_updates = 256
feed = { interval = "500ms", lag = "2s", noise = 0.5 }

[[agents]]
type = "zero_intelligence"
name = "noise"
count = 30
limit_rate = 1.0
market_rate = 0.2
cancel_rate = 0.3
max_offset = 4
latency = { to_exchange = "300us", from_exchange = "300us", jitter = "100us" }

[[agents]]
type = "market_maker"
name = "maker"
intensity = 1.0
post_only = true
latency = { to_exchange = "20us", from_exchange = "20us" }
account = { max_position = 100, max_order_quantity = 10 }

[[agents]]
type = "informed"
name = "informed"
count = 3
interval = "1s"
noise = 1.0
lag = "1s"
threshold = 2.0
order_size = 2
max_position = 50
latency = { to_exchange = "200us", from_exchange = "200us", jitter = "50us" }
account = { max_position = 60, max_order_quantity = 2 }
)";

// A prediction market on one question, with news.
constexpr std::string_view kPrediction = R"(
seed = 5
duration = "120s"

[prediction]
probability = 0.5
news_rate = 0.05
news_share = 0.5
step = "1s"

[exchange]
depth_levels = 5
taker_fee = 1.75
fee_curve = "probability"

[participant]
latency = { to_exchange = "1ms", from_exchange = "1ms" }
account = { max_position = 20, max_order_quantity = 5 }

[learning]
sizes = [1, 3]
depth_levels = 5
max_events = 1024
max_depth_updates = 256

[[agents]]
type = "zero_intelligence"
name = "noise"
count = 30
limit_rate = 1.0
market_rate = 0.2
cancel_rate = 0.3
max_offset = 3
latency = { to_exchange = "300us", from_exchange = "300us", jitter = "100us" }

[[agents]]
type = "informed"
name = "informed"
count = 4
interval = "1s"
noise = 2.0
threshold = 2.0
order_size = 2
max_position = 50
latency = { to_exchange = "200us", from_exchange = "200us", jitter = "50us" }
account = { max_position = 60, max_order_quantity = 2 }
)";

// The busy market with settings drawn for each episode.
std::string rangedScenario() {
    return std::string{kBusy} + R"(
[learning.ranges]
duration = ["40s", "80s"]
agents.noise.count = [20, 40]
agents.noise.limit_rate = [0.5, 1.5]
fundamental.volatility = { log = [0.2, 1.0] }
exchange.taker_fee = { choices = [0.1, 0.3] }
exchange.maker_fee = { range = [-0.1, 0.0], step = 0.05 }
agents.noise.max_offset = { range = [2, 8], step = 2 }
participant.latency.to_exchange = ["500us", "2ms"]
)";
}

// Buffers for one market's observation.
struct Buffers {
    explicit Buffers(const LearningConfig& config)
        : state(kLearningStateNames.size()), orders(config.maxOrders * kOrderColumns),
          events(config.maxEvents * kEventColumns),
          depth(config.maxDepthUpdates * 4 * config.depthLevels), book(4 * config.depthLevels),
          counts(kCountColumns) {}

    std::vector<std::int64_t> state, orders, events, depth, book, counts;

    LearningObservation view() { return {state, orders, events, depth, book, counts}; }

    [[nodiscard]] std::int64_t field(std::string_view name) const {
        const auto found = std::ranges::find(kLearningStateNames, name);
        return state.at(static_cast<std::size_t>(found - kLearningStateNames.begin()));
    }

    // The rows written: the latest events since the last decision.
    [[nodiscard]] std::vector<std::array<std::int64_t, kEventColumns>> rows() const {
        const auto kept = std::min<std::size_t>(static_cast<std::size_t>(counts[0]),
                                                events.size() / kEventColumns);
        std::vector<std::array<std::int64_t, kEventColumns>> out(kept);
        for (std::size_t i = 0; i < kept; ++i) {
            std::copy_n(events.begin() + static_cast<std::ptrdiff_t>(i * kEventColumns),
                        kEventColumns, out[i].begin());
        }
        return out;
    }
};

std::size_t actionNumber(LearningAction action) {
    return static_cast<std::size_t>(action); // the default list holds every action in order
}

// A market of a scenario's text, reset with the text's seed.
struct Started {
    Started(std::string_view text, EventSink* sink = nullptr)
        : scenario(parseScenario(text)),
          market(scenario.learning.value_or(LearningConfig{}), registry(), sink),
          buffers(market.shape()) {
        first = market.reset(scenario, buffers.view(), std::string{text});
    }

    LearningStep step(LearningAction action, std::size_t size = 0) {
        return market.step(actionNumber(action), size, buffers.view());
    }

    Scenario scenario;
    LearningMarket market;
    Buffers buffers;
    LearningStep first;
};

TEST(LearningConfigTest, ReadsEverySettingAndHasDefaults) {
    const Scenario defaults = parseScenario("[learning]\n[[agents]]\ntype = \"momentum\"\n");
    ASSERT_TRUE(defaults.learning);
    EXPECT_EQ(*defaults.learning, LearningConfig{});
    EXPECT_EQ(defaults.learning->actions.size(), kLearningActions.size());
    EXPECT_FALSE(parseScenario("[[agents]]\ntype = \"momentum\"\n").learning);

    const Scenario scenario = parseScenario(R"(
duration = "90s"
[fundamental]
[learning]
interval = "250ms"
warmup = "5s"
actions = ["hold", "both_join", "close"]
sizes = [2, 10]
take_ticks = 1
reward_mark = "book"
close = "value"
max_events = 64
max_depth_updates = 8
depth_levels = 3
max_orders = 4
feed = { interval = "100ms", lag = "1s", noise = 0.25, trigger = 0.5 }
[learning.ranges]
agents.momentum.threshold = [1, 3]
fundamental.volatility = { log = [0.1, 2] }
exchange.taker_fee = { choices = [0, 0.5] }
participant.latency.jitter = ["0s", "1ms"]
[[agents]]
type = "momentum"
)");
    ASSERT_TRUE(scenario.learning);
    const LearningConfig& config = *scenario.learning;
    EXPECT_EQ(config.interval, 250 * kMillisecond);
    EXPECT_EQ(config.warmup, 5 * kSecond);
    EXPECT_EQ(config.actions, (std::vector<LearningAction>{
                                  LearningAction::Hold, LearningAction::BothJoin,
                                  LearningAction::Close}));
    EXPECT_EQ(config.sizes, (std::vector<Quantity>{2, 10}));
    EXPECT_EQ(config.takeTicks, 1);
    EXPECT_EQ(config.rewardMark, Mark::Book);
    EXPECT_EQ(config.close, Mark::Value);
    EXPECT_EQ(config.maxEvents, 64U);
    EXPECT_EQ(config.maxDepthUpdates, 8U);
    EXPECT_EQ(config.depthLevels, 3U);
    EXPECT_EQ(config.maxOrders, 4U);
    EXPECT_EQ(config.feed, (LearningFeed{.interval = 100 * kMillisecond,
                                         .lag = kSecond,
                                         .noise = 0.25,
                                         .trigger = 0.5}));
    ASSERT_EQ(config.ranges.size(), 4U);
    // In the order of their paths.
    EXPECT_EQ(config.ranges[0].path(), "agents.momentum.threshold");
    EXPECT_EQ(config.ranges[0].low, Parameters::Value{std::int64_t{1}});
    EXPECT_EQ(config.ranges[1].path(), "exchange.taker_fee");
    EXPECT_EQ(config.ranges[1].form, SettingRange::Form::Choices);
    EXPECT_EQ(config.ranges[1].choices,
              (std::vector<Parameters::Value>{std::int64_t{0}, 0.5}));
    EXPECT_EQ(config.ranges[2].path(), "fundamental.volatility");
    EXPECT_EQ(config.ranges[2].form, SettingRange::Form::Log);
    EXPECT_EQ(config.ranges[2].high, Parameters::Value{2.0});
    EXPECT_EQ(config.ranges[3].path(), "participant.latency.jitter");
    EXPECT_EQ(config.ranges[3].high, Parameters::Value{std::string{"1ms"}});
}

TEST(LearningConfigTest, ReportsBadSettings) {
    const std::string agent = "[[agents]]\ntype = \"momentum\"\n";
    const auto error = [&](const std::string& learning) {
        try {
            static_cast<void>(parseScenario("[learning]\n" + learning + "\n" + agent));
        } catch (const ScenarioError& caught) {
            return std::string{caught.what()};
        }
        return std::string{"no error"};
    };
    const std::vector<std::pair<std::string, std::string>> cases{
        {"speed = 2", "unknown key 'speed' in [learning]"},
        {"interval = \"0s\"", "'interval' must be positive"},
        {"warmup = \"60s\"", "must come before the end of the run"},
        {"actions = []", "'actions' must be a list of one or more"},
        {"actions = [\"jump\"]", "there is no action 'jump'"},
        {"actions = [\"hold\", \"hold\"]", "action 'hold' is listed twice"},
        {"sizes = [0]", "'sizes' must be between 1"},
        {"reward_mark = \"mid\"", "'reward_mark' must be \"last\", \"value\" or \"book\""},
        {"close = \"value\"", "valuing at the true value needs"},
        {"max_events = 0", "'max_events' must be between 1"},
        {"feed = { lag = \"1s\" }", "a feed of the underlying needs"},
        {"ranges = { duration = [\"1s\"] }", "the range for 'duration' must be [low, high]"},
        {"ranges = { duration = [\"1s\", 3] }", "must run between two numbers or two durations"},
        {"ranges = { duration = [\"3s\", \"1s\"] }", "must not run from high to low"},
        {"ranges = { duration = [\"3s\", \"1q\"] }", "'duration':"},
        {"ranges = { seed = { log = [0, 2] } }", "between two positive numbers"},
        {"ranges = { seed = { choices = [] } }", "a list of one or more values"},
        {"ranges = { seed = { log = [1, 2], choices = [1] } }", "one of range with a step"},
        {"ranges = { seed = { range = [1, 2] } }", "missing 'step'"},
        {"ranges = { seed = { log = [1, 2], step = 1 } }", "one of range with a step"},
        {"ranges = { seed = { range = [1, 2], step = 0 } }", "must be positive"},
        {"ranges = { seed = { range = [1, 2], step = \"1s\" } }", "must be a number for"},
        {"ranges = { seed = 3 }", "the range for 'seed' must be [low, high]"},
    };
    for (const auto& [learning, expected] : cases) {
        EXPECT_NE(error(learning).find(expected), std::string::npos)
            << "for " << learning << ": " << error(learning);
    }
    // A feed comes at most every millisecond, and its trigger is not negative.
    std::string triggered{kBusy};
    const std::string_view noise = "noise = 0.5 }";
    triggered.replace(triggered.find(noise), noise.size(), "noise = 0.5, trigger = -1 }");
    try {
        static_cast<void>(parseScenario(triggered));
        ADD_FAILURE() << "a negative trigger was accepted";
    } catch (const ScenarioError& caught) {
        EXPECT_NE(std::string{caught.what()}.find("'trigger' must not be negative"),
                  std::string::npos);
    }
    std::string fed{kBusy};
    const std::string_view every = "interval = \"500ms\", lag";
    fed.replace(fed.find(every), every.size(), "interval = \"500us\", lag");
    try {
        static_cast<void>(parseScenario(fed));
        ADD_FAILURE() << "a feed every 500us was accepted";
    } catch (const ScenarioError& caught) {
        EXPECT_NE(std::string{caught.what()}.find("at least 1ms"), std::string::npos);
    }
}

TEST(LearningTest, ActionsQuoteAndTakeAtTheBestPricesOthersShow) {
    // The first request each action sends from a fresh market: the book is 99 and 98 bid, 101
    // and 102 offered, 4 lots each; sizes 1, 3 and 8, at most 5 lots an order.
    const auto first = [](LearningAction action, std::size_t size) {
        Started started{kScripted};
        EXPECT_EQ(started.buffers.field("bid_price"), 99);
        EXPECT_EQ(started.buffers.field("ask_price"), 101);
        started.step(action, size);
        return started.market.session().actions;
    };
    const auto quote = [](Side side, Price price, Quantity quantity) {
        return Request{NewOrder{.side = side,
                                .timeInForce = TimeInForce::PostOnly,
                                .price = price,
                                .quantity = quantity}};
    };
    const auto take = [](Side side, Price price, Quantity quantity) {
        return Request{NewOrder{.side = side,
                                .timeInForce = TimeInForce::ImmediateOrCancel,
                                .price = price,
                                .quantity = quantity}};
    };
    // Requests as sent, client order ids left out.
    const auto requests = [](const std::vector<SessionAction>& actions) {
        std::vector<Request> sent;
        for (const SessionAction& action : actions) {
            EXPECT_EQ(action.time, kSecond);
            Request request = action.request;
            if (auto* order = std::get_if<NewOrder>(&request)) {
                order->clientOrderId = 0;
            }
            sent.push_back(request);
        }
        return sent;
    };
    EXPECT_TRUE(first(LearningAction::Hold, 0).empty());
    EXPECT_EQ(requests(first(LearningAction::BidJoin, 1)),
              (std::vector<Request>{quote(Side::Buy, 99, 3)}));
    EXPECT_EQ(requests(first(LearningAction::AskImprove, 0)),
              (std::vector<Request>{quote(Side::Sell, 100, 1)}));
    EXPECT_EQ(requests(first(LearningAction::BidBehind, 0)),
              (std::vector<Request>{quote(Side::Buy, 98, 1)}));
    EXPECT_EQ(requests(first(LearningAction::AskBehind, 2)), // 8 lots, cut to 5
              (std::vector<Request>{quote(Side::Sell, 102, 5)}));
    // Two ticks apart, improving both would put them on one price: both join.
    EXPECT_EQ(requests(first(LearningAction::BothImprove, 0)),
              (std::vector<Request>{quote(Side::Buy, 99, 1), quote(Side::Sell, 101, 1)}));
    EXPECT_EQ(requests(first(LearningAction::BothBehind, 0)),
              (std::vector<Request>{quote(Side::Buy, 98, 1), quote(Side::Sell, 102, 1)}));
    EXPECT_EQ(requests(first(LearningAction::Buy, 1)),
              (std::vector<Request>{take(Side::Buy, 101, 3)}));
    EXPECT_EQ(requests(first(LearningAction::Sell, 0)),
              (std::vector<Request>{take(Side::Sell, 99, 1)}));
    // Nothing to cancel or close.
    EXPECT_TRUE(first(LearningAction::CancelAll, 0).empty());
    EXPECT_TRUE(first(LearningAction::Close, 0).empty());
}

TEST(LearningTest, TheSeatsOwnOrderIsNotTheBestPriceItQuotesAgainst) {
    // The seat's bid at 100 is alone at the top: a tick behind the best bid is a tick behind
    // others' best, 99, not behind its own; and joining goes back to 99.
    Started started{kScripted};
    started.step(LearningAction::BidImprove, 0);
    EXPECT_EQ(started.buffers.field("bid_price"), 100);
    started.step(LearningAction::BidBehind, 0);
    started.step(LearningAction::Hold);
    EXPECT_EQ(started.buffers.field("bid_price"), 99);
    std::vector<SessionAction> sent = started.market.session().actions;
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(sent[1].request,
              (Request{ModifyOrder{.clientOrderId = 1, .price = 98, .quantity = 1}}));
    started.step(LearningAction::BidImprove, 0); // others' best is 99: back to 100
    started.step(LearningAction::BidJoin, 0);    // alone there again: join others at 99
    sent = started.market.session().actions;
    ASSERT_EQ(sent.size(), 4U);
    EXPECT_EQ(sent[2].request,
              (Request{ModifyOrder{.clientOrderId = 1, .price = 100, .quantity = 1}}));
    EXPECT_EQ(sent[3].request,
              (Request{ModifyOrder{.clientOrderId = 1, .price = 99, .quantity = 1}}));
}

TEST(LearningTest, TakingAndClosingGoPastTheSeatsOwnOrders) {
    // The seat's bid of 3 at 100 is alone at the top until the hitter sells 2 into it at 5.5s:
    // closing cancels the lot left and sells the 2 to others at 99, not at its own 100.
    Started closing{kScripted};
    closing.step(LearningAction::BidImprove, 1);
    while (closing.buffers.field("position") == 0) {
        closing.step(LearningAction::Hold);
    }
    EXPECT_EQ(closing.buffers.field("position"), 2);
    closing.step(LearningAction::Close);
    closing.step(LearningAction::Hold);
    EXPECT_EQ(closing.buffers.field("position"), 0);
    std::vector<SessionAction> sent = closing.market.session().actions;
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(sent[1].request, (Request{CancelOrder{.clientOrderId = 1}}));
    EXPECT_EQ(sent[2].request, (Request{NewOrder{.clientOrderId = 2,
                                                 .side = Side::Sell,
                                                 .timeInForce = TimeInForce::ImmediateOrCancel,
                                                 .price = 99,
                                                 .quantity = 2}}));

    // With its own offer alone at 100, buying cancels it and takes others' 101, where the book
    // would otherwise meet the seat's own offer first and turn the order away.
    Started buying{kScripted};
    buying.step(LearningAction::AskImprove, 0);
    buying.step(LearningAction::Buy, 0);
    buying.step(LearningAction::Hold);
    EXPECT_EQ(buying.buffers.field("position"), 1);
    sent = buying.market.session().actions;
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(sent[1].request, (Request{CancelOrder{.clientOrderId = 1}}));
    EXPECT_EQ(sent[2].request, (Request{NewOrder{.clientOrderId = 2,
                                                 .side = Side::Buy,
                                                 .timeInForce = TimeInForce::ImmediateOrCancel,
                                                 .price = 101,
                                                 .quantity = 1}}));
}

TEST(LearningTest, DecisionsComeEveryIntervalAndWhenTheSeatsOrdersFill) {
    Started started{kScripted};
    Buffers& buffers = started.buffers;
    EXPECT_EQ(started.market.now(), kSecond);
    EXPECT_EQ(started.first.elapsed, kSecond);
    EXPECT_EQ(started.first.score, 0);
    EXPECT_TRUE(std::isnan(started.first.value)); // no true value here
    EXPECT_EQ(buffers.field("time"), kSecond);
    EXPECT_EQ(buffers.field("time_left"), 19 * kSecond);
    EXPECT_EQ(buffers.field("bid_quantity"), 4);
    EXPECT_EQ(buffers.field("reference_price"), 100);
    EXPECT_EQ(buffers.field("max_position"), 10);
    EXPECT_EQ(buffers.field("max_order_quantity"), 5);
    // The book as the seat heard it being built: the first bid and offer changed the best
    // prices, and all four the depth.
    EXPECT_EQ(buffers.counts[0], 6);
    EXPECT_EQ(buffers.counts[1], 4);
    EXPECT_EQ(buffers.rows().front()[0], static_cast<std::int64_t>(LearningEvent::Quote));
    EXPECT_EQ(buffers.rows().front()[1], 1'100'000); // 100us to the exchange, 1ms back

    // A bid for 3 at 99 is accepted a round trip later, and the bid shows 7.
    LearningStep step = started.step(LearningAction::BidJoin, 1);
    EXPECT_EQ(step.elapsed, kSecond);
    EXPECT_EQ(step.reward, 0.0);
    EXPECT_FALSE(step.terminated);
    EXPECT_EQ(buffers.rows().front(),
              (std::array<std::int64_t, kEventColumns>{
                  static_cast<std::int64_t>(LearningEvent::Accepted), kSecond + 2'000'000, 1, 99,
                  3, 0, 0}));
    EXPECT_EQ(buffers.field("bid_quantity"), 7);
    EXPECT_EQ(buffers.field("open_buy"), 3);
    EXPECT_EQ(buffers.counts[2], 1);
    EXPECT_EQ(std::vector<std::int64_t>(buffers.orders.begin(), buffers.orders.begin() + 5),
              (std::vector<std::int64_t>{1, 99, 3, 1, 0}));

    // Improving moves it to 100, now 1 lot; improving again leaves it alone at the top.
    started.step(LearningAction::BidImprove, 0);
    EXPECT_EQ(buffers.field("bid_price"), 100);
    started.step(LearningAction::BidImprove, 0);
    started.step(LearningAction::Hold);
    const std::vector<SessionAction> sent = started.market.session().actions;
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(sent[1].time, 2 * kSecond);
    EXPECT_EQ(sent[1].request,
              (Request{ModifyOrder{.clientOrderId = 1, .price = 100, .quantity = 1}}));

    // At 5.5s a market order sells 2: the seat's bid fills and it hears so 1ms later, a decision
    // of its own. Closing through the book now would sell at 99 and pay 0.5: a reward of -1.5.
    step = started.step(LearningAction::Hold);
    EXPECT_EQ(started.market.now(), 5'501'000'000);
    EXPECT_EQ(step.elapsed, 501'000'000);
    EXPECT_DOUBLE_EQ(step.reward, -1.5);
    EXPECT_EQ(step.score, -1'500);
    const auto rows = buffers.rows();
    const auto fill = std::ranges::find_if(rows, [](const auto& row) {
        return row[0] == static_cast<std::int64_t>(LearningEvent::Fill);
    });
    ASSERT_NE(fill, rows.end());
    EXPECT_EQ(*fill, (std::array<std::int64_t, kEventColumns>{
                         static_cast<std::int64_t>(LearningEvent::Fill), 5'501'000'000, 1, 100, 1,
                         0, 0}));
    EXPECT_EQ(buffers.field("position"), 1);
    EXPECT_EQ(buffers.field("cash"), -100);

    // Closing sells it at 99, and the fill is the next decision, a round trip later.
    step = started.step(LearningAction::Close);
    EXPECT_EQ(step.elapsed, 2'000'000);
    EXPECT_EQ(step.reward, 0.0);
    EXPECT_EQ(buffers.field("position"), 0);
    EXPECT_EQ(buffers.field("fees"), 500);
    EXPECT_EQ(started.market.session().actions.back().request,
              (Request{NewOrder{.clientOrderId = 2,
                                .side = Side::Sell,
                                .timeInForce = TimeInForce::ImmediateOrCancel,
                                .price = 99,
                                .quantity = 1}}));

    // Then a decision every second to the end.
    Timestamp expected = 6 * kSecond;
    while (!step.terminated) {
        step = started.step(LearningAction::Hold);
        EXPECT_EQ(started.market.now(), expected);
        expected += kSecond;
    }
    EXPECT_EQ(started.market.now(), 20 * kSecond);
    EXPECT_EQ(step.score, -1'500);
    EXPECT_EQ(buffers.field("time_left"), 0);
    EXPECT_TRUE(started.market.finished());
    EXPECT_THROW(started.step(LearningAction::Hold), std::logic_error);
    EXPECT_THROW(static_cast<void>(started.market.step(kLearningActions.size(), 0, buffers.view())),
                 std::logic_error);
}

TEST(LearningTest, ActionsAndSizesOutOfRangeAreRefused) {
    Started started{kScripted};
    EXPECT_THROW(static_cast<void>(started.market.step(16, 0, started.buffers.view())),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(started.market.step(0, 3, started.buffers.view())),
                 std::invalid_argument);
}

// A policy that picks actions at random, the same for the same seed.
class RandomPolicy {
public:
    explicit RandomPolicy(std::uint64_t seed) : random_(seed, 1) {}

    std::pair<std::size_t, std::size_t> next() {
        const auto action = random_.below(kLearningActions.size());
        const auto size = random_.below(2);
        return {static_cast<std::size_t>(action), static_cast<std::size_t>(size)};
    }

private:
    Random random_;
};

// Runs a market to its end at random and returns the sum of its rewards.
double playOut(Started& started, std::uint64_t seed) {
    RandomPolicy policy{seed};
    double rewards = 0.0;
    for (LearningStep step = started.first; !step.terminated;) {
        const auto [action, size] = policy.next();
        step = started.market.step(action, size, started.buffers.view());
        rewards += step.reward;
    }
    return rewards;
}

// The score the engine's scorer gives the episode as a session, at the given mark.
Score replayedScore(const Session& session, std::string_view mark) {
    Session scored = session;
    scored.scenario += std::string{"\n[scoring]\nmark = \""} + std::string{mark} + "\"\n";
    const RunResult result = replaySession(scored, registry());
    EXPECT_EQ(result.scores.size(), 1U);
    return result.scores.at(0).score;
}

// A session as its file's text, to compare sessions.
std::string sessionText(const Session& session) {
    std::ostringstream out;
    writeSession(out, session);
    return out.str();
}

// Takes decision `step` in a market: an action at random on even steps, explicit orders around
// the best prices on odd ones, so that a copy has both kinds of answer to take again.
LearningStep decide(LearningMarket& market, Buffers& buffers, RandomPolicy& policy,
                    std::size_t step) {
    if (step % 2 == 0) {
        const auto [action, size] = policy.next();
        return market.step(action, size, buffers.view());
    }
    const std::int64_t bid = buffers.field("bid_price");
    const std::int64_t ask = buffers.field("ask_price");
    return market.stepOrders({.bidPrice = bid > 1 ? bid - 1 : 0,
                              .bidSize = 2,
                              .askPrice = ask > 0 ? ask + 1 : 0,
                              .askSize = 2,
                              .take = step % 3 == 0 ? 1 : 0},
                             buffers.view());
}

bool sameReport(const LearningStep& a, const LearningStep& b) {
    const auto same = [](double x, double y) { return x == y || (std::isnan(x) && std::isnan(y)); };
    return same(a.reward, b.reward) && a.terminated == b.terminated && a.stopped == b.stopped &&
           a.elapsed == b.elapsed && a.score == b.score && same(a.value, b.value);
}

bool sameObservation(const Buffers& a, const Buffers& b) {
    return a.state == b.state && a.orders == b.orders && a.events == b.events &&
           a.depth == b.depth && a.book == b.book && a.counts == b.counts;
}

TEST(LearningTest, ACopyGoesOnAsTheOriginalDoes) {
    Started original{kBusy};
    RandomPolicy policy{3};
    LearningStep last = original.first;
    for (std::size_t step = 0; step < 15; ++step) {
        last = decide(original.market, original.buffers, policy, step);
    }
    ASSERT_FALSE(last.terminated);

    LearningMarket copy{original.market.shape(), registry()};
    Buffers copied{copy.shape()};
    EXPECT_TRUE(sameReport(copy.copyFrom(original.market, copied.view()), last));
    EXPECT_TRUE(sameObservation(copied, original.buffers));
    EXPECT_EQ(copy.now(), original.market.now());

    // A second copy, taken somewhere else, changes nothing in the original.
    const std::string before = sessionText(original.market.session());
    const std::vector<std::int64_t> stateBefore = original.buffers.state;
    LearningMarket other{original.market.shape(), registry()};
    Buffers otherBuffers{other.shape()};
    other.copyFrom(original.market, otherBuffers.view());
    RandomPolicy elsewhere{99};
    for (std::size_t step = 0; step < 10 && !other.finished(); ++step) {
        decide(other, otherBuffers, elsewhere, step);
    }
    EXPECT_EQ(sessionText(original.market.session()), before);
    EXPECT_EQ(original.buffers.state, stateBefore);
    EXPECT_EQ(original.market.now(), copy.now());

    // The original and the first copy, given the same answers, go on alike to the end.
    RandomPolicy forOriginal{7};
    RandomPolicy forCopy{7};
    for (std::size_t step = 15; !original.market.finished(); ++step) {
        const LearningStep a = decide(original.market, original.buffers, forOriginal, step);
        const LearningStep b = decide(copy, copied, forCopy, step);
        ASSERT_TRUE(sameReport(a, b)) << "at step " << step;
        ASSERT_TRUE(sameObservation(original.buffers, copied)) << "at step " << step;
    }
    EXPECT_TRUE(copy.finished());
    EXPECT_EQ(sessionText(copy.session()), sessionText(original.market.session()));

    // A finished episode copies as finished, with the same last report and session.
    LearningMarket ended{original.market.shape(), registry()};
    Buffers endedBuffers{ended.shape()};
    const LearningStep endedReport = ended.copyFrom(original.market, endedBuffers.view());
    EXPECT_TRUE(endedReport.terminated);
    EXPECT_TRUE(ended.finished());
    EXPECT_TRUE(sameObservation(endedBuffers, original.buffers));
    EXPECT_EQ(sessionText(ended.session()), sessionText(original.market.session()));
}

// A scenario without a [learning] section takes its timing from the market's shape, so a copy
// into a market whose shape times decisions otherwise must still take the source's.
TEST(LearningTest, ACopyKeepsTheSourcesTimingWhateverItsOwnShapeSays) {
    Scenario scenario = parseScenario(kBusy);
    const LearningConfig sourceShape = *scenario.learning;
    scenario.learning.reset();
    LearningMarket source{sourceShape, registry()};
    Buffers sourceBuffers{source.shape()};
    source.reset(scenario, sourceBuffers.view(), std::string{kBusy});
    RandomPolicy policy{5};
    for (std::size_t step = 0; step < 5; ++step) {
        decide(source, sourceBuffers, policy, step);
    }
    LearningConfig slower = sourceShape;
    slower.interval = 3 * kSecond;
    slower.warmup = 2 * kSecond;
    LearningMarket copy{slower, registry()};
    Buffers copied{copy.shape()};
    copy.copyFrom(source, copied.view());
    EXPECT_EQ(copy.now(), source.now());
    EXPECT_TRUE(sameObservation(copied, sourceBuffers));
    // And it goes on deciding at the source's interval.
    RandomPolicy forSource{8};
    RandomPolicy forCopy{8};
    for (std::size_t step = 5; step < 10; ++step) {
        ASSERT_TRUE(sameReport(decide(source, sourceBuffers, forSource, step),
                               decide(copy, copied, forCopy, step)))
            << "at step " << step;
    }
    EXPECT_EQ(copy.now(), source.now());
}

TEST(LearningTest, ACopyNeedsAnEpisodeOfTheSameShape) {
    Started original{kBusy};
    LearningMarket empty{original.market.shape(), registry()};
    Buffers buffers{empty.shape()};
    EXPECT_THROW(original.market.copyFrom(original.market, original.buffers.view()),
                 std::invalid_argument);
    EXPECT_THROW(original.market.copyFrom(empty, original.buffers.view()), std::logic_error);
    LearningConfig other = original.market.shape();
    other.sizes = {1};
    LearningMarket narrower{other, registry()};
    Buffers narrowerBuffers{narrower.shape()};
    EXPECT_THROW(narrower.copyFrom(original.market, narrowerBuffers.view()),
                 std::invalid_argument);
    EXPECT_FALSE(narrower.started());
}

TEST(LearningTest, RewardsAddUpToTheScoreClosedThroughTheBook) {
    Started started{kBusy};
    // The busy market has a true value, which rewards use between decisions.
    EXPECT_FALSE(std::isnan(started.first.value));
    const double rewards = playOut(started, 21);
    const Session session = started.market.session();
    EXPECT_GT(session.actions.size(), 20U);
    const Score score = replayedScore(session, "book");
    EXPECT_NEAR(rewards, static_cast<double>(score.total) / 1'000.0, 1e-6);
    EXPECT_NE(score.total, 0);
}

TEST(LearningTest, RewardsCountFromTheStartWhenTheSeatStartsWithAScore) {
    // A target's penalty, or a position held from the start, gives the seat a score before its
    // first decision: the first reward counts it, so the rewards still add up to the score.
    const auto holding = [](const std::string& text) {
        Started started{text};
        EXPECT_NE(started.first.score, 0);
        double rewards = 0.0;
        LearningStep step = started.first;
        while (!step.terminated) {
            step = started.step(LearningAction::Hold);
            rewards += step.reward;
        }
        EXPECT_NEAR(rewards * 1'000.0, static_cast<double>(step.score), 1e-6);
    };
    holding(std::string{kScripted} +
            "\n[scoring]\ntarget = { side = \"buy\", quantity = 5, unfinished_penalty = 1 }\n");
    std::string positioned{kScripted};
    const std::string_view account = "account = { max_position = 10";
    positioned.replace(positioned.find(account), account.size(),
                       "account = { initial_position = 3, max_position = 10");
    holding(positioned + "\n[scoring]\nclose_penalty = 1\ninventory_penalty = 0.1\n");
}

TEST(LearningTest, AResetThatIsRefusedLeavesTheEpisodeThereWas) {
    Started started{kScripted};
    started.step(LearningAction::Hold);
    const Timestamp now = started.market.now();
    // Other sizes than the market's, and buffers of other sizes, are refused before anything
    // changes, and the episode goes on.
    std::string resized{kScripted};
    resized.replace(resized.find("sizes = [1, 3, 8]"), 17, "sizes = [1, 3]");
    EXPECT_THROW(static_cast<void>(started.market.reset(parseScenario(resized),
                                                        started.buffers.view())),
                 ScenarioError);
    LearningConfig smaller = started.market.shape();
    smaller.maxEvents = 4;
    Buffers small{smaller};
    EXPECT_THROW(static_cast<void>(started.market.step(0, 0, small.view())),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(started.market.reset(started.scenario, small.view())),
                 std::invalid_argument);
    EXPECT_EQ(started.market.now(), now);
    started.step(LearningAction::Hold);
    EXPECT_EQ(started.market.now(), now + kSecond);
}

TEST(LearningTest, APredictionMarketIsValuedAtResolutionAndRewardedAtTheValue) {
    Started started{kPrediction};
    const std::shared_ptr<Fundamental> value = makeFundamental(started.scenario);
    EXPECT_DOUBLE_EQ(started.first.value, value->valueAt(started.market.now()));
    EXPECT_EQ(started.buffers.field("max_price"), 99);
    RandomPolicy policy{8};
    double rewards = 0.0;
    for (LearningStep step = started.first; !step.terminated;) {
        const auto [action, size] = policy.next();
        step = started.market.step(action, size, started.buffers.view());
        rewards += step.reward;
        EXPECT_DOUBLE_EQ(step.value, value->valueAt(started.market.now()));
    }
    const double resolution = value->valueAt(started.market.now());
    EXPECT_TRUE(resolution == 0.0 || resolution == 100.0);
    const Score score = replayedScore(started.market.session(), "value");
    EXPECT_NEAR(rewards, static_cast<double>(score.total) / 1'000.0, 1e-6);
}

TEST(LearningTest, TheEventStreamKeepsTheLatestEventsInTimeOrder) {
    // The same market twice, with room for every event and with room for 6 and 2 depth updates.
    const Scenario scenario = parseScenario(kBusy);
    Scenario cramped = scenario;
    cramped.learning->maxEvents = 6;
    cramped.learning->maxDepthUpdates = 2;
    LearningMarket wide{*scenario.learning, registry()};
    LearningMarket narrow{*cramped.learning, registry()};
    Buffers all{wide.shape()};
    Buffers few{narrow.shape()};
    wide.reset(scenario, all.view());
    narrow.reset(cramped, few.view());
    RandomPolicy policy{4};
    const std::size_t levels = 4 * scenario.learning->depthLevels;
    for (int decision = 0; decision < 40; ++decision) {
        EXPECT_EQ(all.counts, few.counts);
        ASSERT_LE(all.counts[0], 1'024);
        ASSERT_LE(all.counts[1], 256);
        const auto everything = all.rows();
        const auto latest = few.rows();
        ASSERT_EQ(latest.size(), std::min<std::size_t>(everything.size(), 6));
        for (std::size_t i = 0; i < latest.size(); ++i) {
            auto row = everything[everything.size() - latest.size() + i];
            if (i > 0) {
                EXPECT_LE(latest[i - 1][1], latest[i][1]); // in time order
            }
            if (row[0] == static_cast<std::int64_t>(LearningEvent::Depth)) {
                // The narrow market kept the last two updates; the row says which, or -1.
                const std::int64_t kept = all.counts[1] - std::min<std::int64_t>(all.counts[1], 2);
                const std::int64_t at = row[2] >= kept ? row[2] - kept : -1;
                EXPECT_EQ(latest[i][2], at);
                if (at >= 0) {
                    EXPECT_TRUE(std::equal(
                        few.depth.begin() + at * static_cast<std::ptrdiff_t>(levels),
                        few.depth.begin() + (at + 1) * static_cast<std::ptrdiff_t>(levels),
                        all.depth.begin() + row[2] * static_cast<std::ptrdiff_t>(levels)));
                }
                row[2] = latest[i][2];
            }
            EXPECT_EQ(latest[i], row);
        }
        EXPECT_EQ(all.state, few.state);
        EXPECT_EQ(all.book, few.book);
        const auto [action, size] = policy.next();
        wide.step(action, size, all.view());
        narrow.step(action, size, few.view());
    }
}

TEST(LearningTest, TheFeedShowsTheUnderlyingLateWithFreshNoise) {
    // Feed rows over a minute, with the noise as given or none.
    const auto feed = [](double noise) {
        Scenario scenario = parseScenario(kBusy);
        scenario.learning->feed->noise = noise;
        LearningMarket market{*scenario.learning, registry()};
        Buffers buffers{market.shape()};
        market.reset(scenario, buffers.view());
        std::vector<std::array<std::int64_t, kEventColumns>> rows;
        for (;;) {
            EXPECT_LE(buffers.counts[0], 1'024);
            for (const auto& row : buffers.rows()) {
                if (row[0] == static_cast<std::int64_t>(LearningEvent::Feed)) {
                    rows.push_back(row);
                }
            }
            if (market.finished()) {
                return rows;
            }
            market.step(0, 0, buffers.view());
        }
    };
    const auto clean = feed(0.0);
    ASSERT_EQ(clean.size(), 121U); // every 500ms from 0 to 60s
    const std::shared_ptr<Fundamental> value = makeFundamental(parseScenario(kBusy));
    for (std::size_t i = 0; i < clean.size(); ++i) {
        const Timestamp time = static_cast<Timestamp>(i) * 500 * kMillisecond;
        EXPECT_EQ(clean[i][1], time);
        static_cast<void>(value->valueAt(std::max<Timestamp>(time - 2 * kSecond, 0)));
        EXPECT_EQ(clean[i][2], std::llround(value->walk() * 1e6)) << "at " << time;
    }
    const auto noisy = feed(0.5);
    EXPECT_EQ(noisy, feed(0.5));
    ASSERT_EQ(noisy.size(), clean.size());
    double squares = 0.0;
    for (std::size_t i = 0; i < noisy.size(); ++i) {
        const double error = static_cast<double>(noisy[i][2] - clean[i][2]) / 1e6;
        squares += error * error;
    }
    EXPECT_NEAR(std::sqrt(squares / static_cast<double>(noisy.size())), 0.5, 0.15);
}

// A feed of the value publishes the true value, cents of the payout in a game's market, not the
// score behind it.
TEST(LearningTest, AFeedOfTheValuePublishesTheValue) {
    const std::string text = R"(duration = "60s"
[prediction]
probability = 0.4
[prediction.game]
length = "50s"
scores = 6
[learning]
feed = { interval = "1s", lag = "2s", of = "value" }
[[agents]]
type = "zero_intelligence"
count = 10
)";
    Scenario scenario = parseScenario(text);
    ASSERT_TRUE(scenario.learning->feed->value);
    LearningMarket market{*scenario.learning, registry()};
    Buffers buffers{market.shape()};
    market.reset(scenario, buffers.view());
    const std::shared_ptr<Fundamental> value = makeFundamental(parseScenario(text));
    std::size_t seen = 0;
    while (!market.finished()) {
        for (const auto& row : buffers.rows()) {
            if (row[0] == static_cast<std::int64_t>(LearningEvent::Feed)) {
                const Timestamp time = row[1];
                EXPECT_EQ(
                    row[2],
                    std::llround(value->valueAt(std::max<Timestamp>(time - 2 * kSecond, 0)) * 1e6))
                    << "at " << time;
                ++seen;
            }
        }
        market.step(0, 0, buffers.view());
    }
    EXPECT_GT(seen, 30U);
    std::string wrong = text;
    wrong.replace(wrong.find("\"value\""), 7, "\"price\"");
    EXPECT_THROW(static_cast<void>(parseScenario(wrong)), ScenarioError);
}

// A busy market's feed, every 500ms and 2s late, without noise and with the trigger given.
std::string triggeredFeed(double trigger) {
    std::string text{kBusy};
    const std::string_view noise = "noise = 0.5 }";
    text.replace(text.find(noise), noise.size(),
                 std::format("noise = 0.0, trigger = {} }}", trigger));
    return text;
}

TEST(LearningTest, TheFeedsTriggerCallsADecisionWhenItMoves) {
    // Holding throughout, the decisions' times, the feed each had heard last, and the market's
    // log, which holding leaves the same however often the seat decides.
    struct Run {
        std::vector<Timestamp> times;
        std::vector<std::int64_t> heard;
        std::vector<std::vector<std::int64_t>> published; // each step's feed rows
        std::string log;
    };
    const auto run = [](double trigger) {
        Run found;
        std::ostringstream out;
        CsvEventLog log{out};
        const Scenario scenario = parseScenario(triggeredFeed(trigger));
        LearningMarket market{*scenario.learning, registry(), &log};
        Buffers buffers{market.shape()};
        market.reset(scenario, buffers.view());
        for (;;) {
            std::vector<std::int64_t> rows;
            for (const auto& row : buffers.rows()) {
                if (row[0] == static_cast<std::int64_t>(LearningEvent::Feed)) {
                    rows.push_back(row[2]);
                }
            }
            found.times.push_back(market.now());
            found.heard.push_back(rows.empty() ? found.heard.back() : rows.back());
            found.published.push_back(rows);
            if (market.finished()) {
                break;
            }
            market.step(0, 0, buffers.view());
        }
        found.log = out.str();
        return found;
    };
    const Run plain = run(0.0);
    EXPECT_EQ(plain.times.size(), 60U); // every second from 1s to the end at 60s
    const double trigger = 0.6;
    const Run triggered = run(trigger);
    EXPECT_EQ(triggered.log, plain.log);
    EXPECT_GT(triggered.times.size(), plain.times.size() + 5);
    const auto units = static_cast<std::int64_t>(trigger * 1e6);
    for (std::size_t i = 1; i < triggered.times.size(); ++i) {
        const Timestamp time = triggered.times[i];
        const std::int64_t before = triggered.heard[i - 1];
        const bool moved = std::abs(triggered.heard[i] - before) >= units;
        if (time % kSecond != 0) {
            // Off the second, only a publication that moved the feed calls a decision.
            EXPECT_EQ(time % (500 * kMillisecond), 0) << "at " << time;
            EXPECT_TRUE(moved) << "at " << time;
        }
        // No publication before the last moved the feed from what the last decision heard.
        const std::vector<std::int64_t>& rows = triggered.published[i];
        for (std::size_t r = 0; r + 1 < rows.size(); ++r) {
            EXPECT_LT(std::abs(rows[r] - before), units) << "at " << time;
        }
    }
}

TEST(LearningTest, AnEpisodeReplaysAsASessionByteForByte) {
    const auto check = [](const std::string& text, std::optional<std::uint64_t> settingsSeed) {
        std::ostringstream live;
        CsvEventLog liveLog{live};
        const EpisodeScenarios scenarios{text, "test", registry()};
        EpisodeScenarios::Drawn drawn = scenarios.draw(settingsSeed.value_or(0));
        drawn.scenario.seed = 77;
        LearningMarket market{scenarios.base().learning.value_or(LearningConfig{}), registry(),
                              &liveLog};
        Buffers buffers{market.shape()};
        market.reset(drawn.scenario, buffers.view(), scenarios.text(settingsSeed.value_or(0)));
        RandomPolicy policy{9};
        for (int decision = 0; decision < 120 && !market.finished(); ++decision) {
            const auto [action, size] = policy.next();
            market.step(action, size, buffers.view());
        }
        const Session session = market.session();
        EXPECT_EQ(session.seed, 77U);
        EXPECT_EQ(session.seats, (std::vector<std::string>{"learner"}));
        // Through a session file and back, then replayed.
        std::ostringstream file;
        writeSession(file, session);
        std::ostringstream replayed;
        CsvEventLog replayLog{replayed};
        static_cast<void>(replaySession(parseSession(file.str()), registry(), &replayLog));
        EXPECT_GT(live.str().size(), 10'000U);
        EXPECT_EQ(replayed.str(), live.str());
    };
    check(std::string{kBusy}, std::nullopt);
    check(rangedScenario(), 12);
    // A seat whose delays are drawn for each message, with a tail.
    std::string drawn{kBusy};
    const std::string fixed = R"(latency = { to_exchange = "1ms", from_exchange = "1ms" })";
    drawn.replace(drawn.find(fixed), fixed.size(),
                  R"(latency = { to_exchange = { quantiles = ["1ms", "2ms", "40ms"] }, )"
                  R"(from_exchange = { quantiles = ["500us", "1ms", "3ms", "90ms"] } })");
    check(drawn, std::nullopt);
    check(triggeredFeed(0.4), std::nullopt);
}

TEST(LearningTest, RangesDrawTheSameSettingsFromTheSameSeedWithinTheirBounds) {
    const EpisodeScenarios scenarios{rangedScenario(), "test", registry()};
    EXPECT_EQ(scenarios.settingNames(),
              (std::vector<std::string>{"agents.noise.count", "agents.noise.limit_rate",
                                        "agents.noise.max_offset", "duration",
                                        "exchange.maker_fee", "exchange.taker_fee",
                                        "fundamental.volatility",
                                        "participant.latency.to_exchange"}));
    std::vector<Fee> makerFees;
    std::vector<std::int64_t> offsets;
    std::vector<double> rates;
    std::vector<double> volatilities;
    std::vector<std::int64_t> counts;
    std::vector<Fee> fees;
    for (std::uint64_t seed = 1; seed <= 300; ++seed) {
        const EpisodeScenarios::Drawn drawn = scenarios.draw(seed);
        const Scenario& scenario = drawn.scenario;
        EXPECT_EQ(scenarios.draw(seed).settings, drawn.settings);
        EXPECT_GE(scenario.duration, 40 * kSecond);
        EXPECT_LE(scenario.duration, 80 * kSecond);
        counts.push_back(scenario.groups[0].count);
        offsets.push_back(scenario.groups[0].parameters.integer("max_offset", 0));
        EXPECT_EQ(drawn.settings[0], static_cast<double>(scenario.groups[0].count));
        const double rate = scenario.groups[0].parameters.number("limit_rate", 0);
        rates.push_back(rate);
        EXPECT_GE(rate, 0.5);
        EXPECT_LT(rate, 1.5);
        fees.push_back(scenario.exchange.takerFee);
        volatilities.push_back(scenario.fundamental->volatility);
        EXPECT_GE(scenario.fundamental->volatility, 0.2);
        EXPECT_LE(scenario.fundamental->volatility, 1.0);
        EXPECT_GE(scenario.participant.latency.toExchange, 500 * kMicrosecond);
        EXPECT_LE(scenario.participant.latency.toExchange, 2 * kMillisecond);
        EXPECT_EQ(drawn.settings[7], static_cast<double>(scenario.participant.latency.toExchange));
        makerFees.push_back(scenario.exchange.makerFee);
        // Everything not drawn is the scenario's own.
        EXPECT_EQ(scenario.groups[1].name, "maker");
        EXPECT_EQ(scenario.exchange.depthLevels, 5U);
    }
    // A stepped range draws each of its three values, and only those.
    std::ranges::sort(makerFees);
    const auto distinct = std::ranges::unique(makerFees);
    makerFees.erase(distinct.begin(), distinct.end());
    EXPECT_EQ(makerFees, (std::vector<Fee>{-100, -50, 0}));
    std::ranges::sort(offsets);
    const auto distinctOffsets = std::ranges::unique(offsets);
    offsets.erase(distinctOffsets.begin(), distinctOffsets.end());
    EXPECT_EQ(offsets, (std::vector<std::int64_t>{2, 4, 6, 8}));
    EXPECT_EQ(std::ranges::min(counts), 20);
    EXPECT_EQ(std::ranges::max(counts), 40);
    // Each range draws from a stream of its own: two ranges' draws are not correlated.
    const auto mean = [](const auto& values) {
        double sum = 0.0;
        for (const auto value : values) {
            sum += static_cast<double>(value);
        }
        return sum / static_cast<double>(values.size());
    };
    const auto correlation = [&](const auto& a, const auto& b) {
        const double meanA = mean(a);
        const double meanB = mean(b);
        double covariance = 0.0;
        double squaresA = 0.0;
        double squaresB = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const double x = static_cast<double>(a[i]) - meanA;
            const double y = static_cast<double>(b[i]) - meanB;
            covariance += x * y;
            squaresA += x * x;
            squaresB += y * y;
        }
        return covariance / std::sqrt(squaresA * squaresB);
    };
    EXPECT_LT(std::abs(correlation(counts, rates)), 0.2);
    EXPECT_LT(std::abs(correlation(rates, volatilities)), 0.2);
    EXPECT_NE(std::ranges::find(fees, 100), fees.end());
    EXPECT_NE(std::ranges::find(fees, 300), fees.end());

    // Another range leaves every other's draw as it was.
    const EpisodeScenarios more{rangedScenario() + "agents.maker.intensity = [0.5, 2.0]\n",
                                "test", registry()};
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        const auto before = scenarios.draw(seed).settings;
        auto after = more.draw(seed).settings;
        after.erase(after.begin()); // agents.maker.intensity comes first
        EXPECT_EQ(after, before);
    }
}

TEST(LearningTest, RangesAreCheckedAtTheirEndsWhenLoaded) {
    const auto error = [](const std::string& ranges) {
        try {
            const EpisodeScenarios scenarios{std::string{kBusy} + "[learning.ranges]\n" + ranges,
                                             "test", registry()};
        } catch (const ScenarioError& caught) {
            return std::string{caught.what()};
        }
        return std::string{"no error"};
    };
    const std::vector<std::pair<std::string, std::string>> cases{
        {"agents.noise.count = [-1, 10]", "every range at its low end"},
        {"agents.noise.count = [1.5, 3.0]", "'count' must be a whole number"},
        {"agents.ghost.count = [1, 2]", "no agent group is named 'ghost'"},
        {"agents.noise = [1, 2]", "must name an agent group and one of its settings"},
        {"prediction.news_rate = [0.1, 0.2]", "the scenario has no table 'prediction'"},
        {"fundamental.wobble = [1, 2]", "unknown key 'wobble' in [fundamental]"},
        {"learning.max_events = [4, 8]", "may not change the actions"},
        {"exchange.taker_fee = [0.1, 0.3]", "drawn with settings seed"},
        {"exchange.fee_curve = { choices = [\"flat\", \"curly\", \"flat\"] }",
         "'exchange.fee_curve' at choice 2"},
        // Checked at 0.3, the highest it draws, not at 0.3005, which no fee may be.
        {"exchange.taker_fee = { range = [0.1, 0.3005], step = 0.05 }", "no error"},
    };
    for (const auto& [ranges, expected] : cases) {
        EXPECT_NE(error(ranges).find(expected), std::string::npos)
            << "for " << ranges << ": " << error(ranges);
    }
}

TEST(LearningTest, TheLossLimitEndsTheEpisode) {
    // Buying 3 at 101 and paying 1.5 in fees is a loss of 1.5 at the last trade: the limit of 1
    // stops the seat at that trade, and the episode ends there, closed through the book.
    Started started{std::string{kScripted} + "\n[scoring]\nmax_loss = 1\n"};
    const LearningStep step = started.step(LearningAction::Buy, 1);
    EXPECT_TRUE(step.terminated);
    EXPECT_TRUE(step.stopped);
    EXPECT_EQ(started.market.now(), kSecond + kMillisecond);
    // Bought 3 at 101, sold back at 99: -6, with 1.5 in fees each way.
    EXPECT_EQ(step.score, -9'000);
    EXPECT_DOUBLE_EQ(step.reward, -9.0);
    EXPECT_TRUE(started.market.finished());
}

// Fills `actions` with each market's next action, the same for the same market and step.
void choose(std::vector<std::int64_t>& actions, std::size_t step) {
    for (std::size_t market = 0; market < actions.size() / 2; ++market) {
        Random random{step, market + 1};
        actions[2 * market] = static_cast<std::int64_t>(random.below(kLearningActions.size()));
        actions[2 * market + 1] = static_cast<std::int64_t>(random.below(2));
    }
}

// Each market's buffers, to compare pools.
struct Snapshot {
    std::vector<std::int64_t> ints;
    std::vector<double> doubles;

    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

Snapshot snapshot(LearningPool& pool, std::size_t market) {
    Snapshot out;
    const std::size_t markets = pool.markets();
    for (const auto buffer : {pool.state(), pool.orders(), pool.events(), pool.depth(), pool.book(),
                              pool.counts(), pool.elapsed(), pool.score()}) {
        const std::size_t each = buffer.size() / markets;
        out.ints.insert(out.ints.end(), buffer.begin() + static_cast<std::ptrdiff_t>(market * each),
                        buffer.begin() + static_cast<std::ptrdiff_t>((market + 1) * each));
    }
    out.ints.push_back(pool.terminated()[market]);
    out.ints.push_back(pool.stopped()[market]);
    out.ints.push_back(static_cast<std::int64_t>(pool.seeds()[market]));
    for (const auto buffer : {pool.rewards(), pool.value(), pool.settings()}) {
        const std::size_t each = buffer.size() / markets;
        for (std::size_t i = market * each; i < (market + 1) * each; ++i) {
            // NaN compares unequal to itself; a sentinel stands for it.
            out.doubles.push_back(std::isnan(buffer[i]) ? -1e300 : buffer[i]);
        }
    }
    return out;
}

// Steps a pool `steps` times, resetting each market whose episode ended with the next seed.
void drive(LearningPool& pool, std::size_t steps, const std::function<void(std::size_t)>& check) {
    const std::size_t markets = pool.markets();
    std::vector<std::uint64_t> seeds(markets);
    for (std::size_t market = 0; market < markets; ++market) {
        seeds[market] = 100 + market;
    }
    pool.reset(seeds);
    check(0);
    std::vector<std::int64_t> actions(2 * markets);
    std::vector<std::uint8_t> mask(markets);
    for (std::size_t step = 1; step <= steps; ++step) {
        choose(actions, step);
        pool.step(actions);
        check(step);
        bool any = false;
        for (std::size_t market = 0; market < markets; ++market) {
            mask[market] = pool.terminated()[market];
            any = any || mask[market] != 0;
            seeds[market] += 1'000;
        }
        if (any) {
            pool.reset(seeds, {}, mask);
        }
        check(step); // after the resets, whether there were any or not

    }
}

std::string shortRanged() {
    // Short episodes, so that markets end and start again while the test runs.
    std::string text = rangedScenario();
    const std::string_view long_ = R"(duration = ["40s", "80s"])";
    text.replace(text.find(long_), long_.size(), R"(duration = ["5s", "9s"])");
    return text;
}

TEST(LearningPoolTest, AMarketsResultsDependOnlyOnItsSeedsAndActions) {
    const std::string text = shortRanged();
    LearningPool alone{text, "test", 5, 1, registry()};
    LearningPool threaded{text, "test", 5, 4, registry()};
    LearningPool fewer{text, "test", 2, 2, registry()};
    EXPECT_EQ(threaded.threads(), 4U);
    EXPECT_EQ(fewer.threads(), 2U);
    std::vector<std::vector<Snapshot>> seen(5);
    drive(alone, 40, [&](std::size_t /*step*/) {
        for (std::size_t market = 0; market < 5; ++market) {
            seen[market].push_back(snapshot(alone, market));
        }
    });
    std::size_t at = 0;
    drive(threaded, 40, [&](std::size_t step) {
        for (std::size_t market = 0; market < 5; ++market) {
            ASSERT_EQ(snapshot(threaded, market), seen[market][at]) << "market " << market
                                                                    << " at step " << step;
        }
        ++at;
    });
    // Two markets on their own, with the same seeds and actions, come out the same as well.
    at = 0;
    drive(fewer, 40, [&](std::size_t step) {
        for (std::size_t market = 0; market < 2; ++market) {
            ASSERT_EQ(snapshot(fewer, market), seen[market][at]) << "market " << market
                                                                 << " at step " << step;
        }
        ++at;
    });
    // Episodes ended and started again along the way.
    EXPECT_GT(seen[0].size(), 41U);

    // And market 3 of the pool is the same market stepped on its own, up to its first end.
    const EpisodeScenarios scenarios{text, "test", registry()};
    EpisodeScenarios::Drawn drawn = scenarios.draw(103);
    drawn.scenario.seed = 103;
    LearningMarket single{scenarios.base().learning.value_or(LearningConfig{}), registry()};
    Buffers buffers{single.shape()};
    single.reset(drawn.scenario, buffers.view());
    const auto same = [&](const Snapshot& pooled) {
        // The pool's state comes first in a snapshot.
        return std::equal(buffers.state.begin(), buffers.state.end(), pooled.ints.begin());
    };
    EXPECT_TRUE(same(seen[3][0]));
    std::vector<std::int64_t> actions(10);
    for (std::size_t step = 1; !single.finished(); ++step) {
        choose(actions, step);
        single.step(static_cast<std::size_t>(actions[6]), static_cast<std::size_t>(actions[7]),
                    buffers.view());
        EXPECT_TRUE(same(seen[3][2 * step - 1])) << "at step " << step;
    }
}

TEST(LearningPoolTest, CopiesAMarketIntoOthersThatThenGoOnAlike) {
    const std::string text = rangedScenario();
    LearningPool pool{text, "test", 4, 2, registry()};
    pool.reset(std::vector<std::uint64_t>{100, 101, 102, 103});
    std::vector<std::int64_t> actions(8);
    for (std::size_t step = 1; step <= 6; ++step) {
        choose(actions, step);
        pool.step(actions);
    }
    const Snapshot untouched = snapshot(pool, 1);
    const std::vector<std::size_t> targets{2, 3};
    pool.copy(0, targets);
    EXPECT_EQ(snapshot(pool, 2), snapshot(pool, 0));
    EXPECT_EQ(snapshot(pool, 3), snapshot(pool, 0));
    EXPECT_EQ(snapshot(pool, 1), untouched);
    EXPECT_EQ(sessionText(pool.session(2)), sessionText(pool.session(0)));

    // Given market 0's actions, the copies go on as it does.
    for (std::size_t step = 7; step <= 12; ++step) {
        choose(actions, step);
        for (const std::size_t market : targets) {
            actions[2 * market] = actions[0];
            actions[2 * market + 1] = actions[1];
        }
        pool.step(actions);
        ASSERT_EQ(snapshot(pool, 2), snapshot(pool, 0)) << "at step " << step;
        ASSERT_EQ(snapshot(pool, 3), snapshot(pool, 0)) << "at step " << step;
    }

    EXPECT_THROW(pool.copy(0, std::vector<std::size_t>{0}), std::invalid_argument);
    EXPECT_THROW(pool.copy(4, targets), std::out_of_range);
    EXPECT_THROW(pool.copy(0, std::vector<std::size_t>{4}), std::out_of_range);
    LearningPool fresh{text, "test", 2, 1, registry()};
    EXPECT_THROW(fresh.copy(0, std::vector<std::size_t>{1}), std::logic_error);
}

TEST(LearningPoolTest, ResetsTheMarketsAskedAndChecksWhatItIsGiven) {
    LearningPool pool{rangedScenario(), "test", 3, 2, registry()};
    const auto n = static_cast<std::ptrdiff_t>(pool.settingNames().size());
    EXPECT_EQ(n, 8);
    EXPECT_THROW(pool.reset(std::vector<std::uint64_t>{1, 2}), std::invalid_argument);
    std::vector<std::int64_t> holds(6, 0);
    EXPECT_THROW(pool.step(holds), std::logic_error); // nothing has started
    pool.reset(std::vector<std::uint64_t>{1, 2, 3}, std::vector<std::uint64_t>{7, 7, 8});
    EXPECT_EQ(std::vector<std::uint64_t>(pool.settingsSeeds().begin(), pool.settingsSeeds().end()),
              (std::vector<std::uint64_t>{7, 7, 8}));
    // The same settings seed draws the same settings, whatever the market's own seed.
    const auto settings = pool.settings();
    EXPECT_TRUE(std::equal(settings.begin(), settings.begin() + n, settings.begin() + n));
    EXPECT_FALSE(std::equal(settings.begin(), settings.begin() + n, settings.begin() + 2 * n));

    pool.step(holds);
    const Snapshot second = snapshot(pool, 1);
    pool.reset(std::vector<std::uint64_t>{4, 5, 6}, {}, std::vector<std::uint8_t>{1, 0, 1});
    EXPECT_EQ(snapshot(pool, 1), second); // left alone
    EXPECT_EQ(pool.seeds()[0], 4U);
    EXPECT_EQ(pool.state()[0], kSecond); // back at the first decision

    std::vector<std::int64_t> bad = holds;
    bad[2] = 16;
    EXPECT_THROW(pool.step(bad), std::invalid_argument);
    bad = holds;
    bad[5] = 2;
    EXPECT_THROW(pool.step(bad), std::invalid_argument);
    EXPECT_THROW(pool.step(std::vector<std::int64_t>(4, 0)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(pool.session(3)), std::out_of_range);
    EXPECT_EQ(pool.session(0).seed, 4U);
}

TEST(LearningPoolTest, AnEpisodeCanRunAScenarioOfTheCallersOwn) {
    LearningPool pool{std::string{kBusy}, "test", 2, 2, registry()};
    std::string prediction{kPrediction};
    // Without a [learning] section of its own, it takes the pool's.
    prediction.erase(prediction.find("[learning]"),
                     prediction.find("[[agents]]") - prediction.find("[learning]"));
    const std::vector<std::optional<std::string>> scenarios{std::nullopt, prediction};
    pool.reset(std::vector<std::uint64_t>{1, 2}, {}, {}, scenarios);
    const std::size_t fields = kLearningStateNames.size();
    EXPECT_EQ(pool.state()[13], 1'000);         // reference price
    EXPECT_EQ(pool.state()[fields + 14], 99);   // highest price
    EXPECT_EQ(pool.session(1).scenario, prediction);
    // A scenario whose observation would have another shape is refused.
    const std::vector<std::optional<std::string>> other{std::string{kScripted}, std::nullopt};
    EXPECT_THROW(pool.reset(std::vector<std::uint64_t>{1, 2}, {}, {}, other), ScenarioError);
}

} // namespace
} // namespace crowdbook

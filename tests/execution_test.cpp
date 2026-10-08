#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/agents/execution.hpp"
#include "crowdbook/event_log.hpp"
#include "crowdbook/scenario.hpp"
#include "crowdbook/scenario_file.hpp"
#include "fake_context.hpp"

namespace crowdbook {
namespace {

using test::FakeContext;

// Parents of exactly twelve lots, worked five at a time every second, a minute apart on average.
const ExecutionConfig kTwap{.style = ExecutionStyle::Twap,
                            .minParent = 12,
                            .maxParent = 12,
                            .pause = 60 * kSecond,
                            .interval = kSecond,
                            .childSize = 5};

// The agent's next wakeup: the clock moves to it and the agent is woken.
void wake(ExecutionTrader& trader, FakeContext& context) {
    ASSERT_FALSE(context.wakeups.empty());
    context.setNow(context.wakeups.back().first);
    trader.onWakeup(context, context.wakeups.back().second);
}

// The one child order sent since the last call.
NewOrder child(FakeContext& context) {
    const std::vector<Request> sent = context.takeSent();
    EXPECT_EQ(sent.size(), 1U);
    return sent.empty() ? NewOrder{} : std::get<NewOrder>(sent.front());
}

// Plays the exchange filling a child order in full.
void fillAll(FakeContext& context, const NewOrder& order) {
    context.accept(order.clientOrderId);
    context.fill(order.clientOrderId, order.quantity);
}

TEST(ExecutionTest, ParentSizesHaveAParetoTail) {
    Random random{1, 0};
    constexpr int kDraws = 200'000;
    std::map<Quantity, int> atLeast{{40, 0}, {100, 0}, {400, 0}};
    for (int i = 0; i < kDraws; ++i) {
        const Quantity size = drawParentSize(random, 20, 1.5, kMaxQuantity);
        ASSERT_GE(size, 20);
        for (auto& [q, count] : atLeast) {
            count += size >= q ? 1 : 0;
        }
    }
    for (const auto& [q, count] : atLeast) {
        const double expected = std::pow(20.0 / static_cast<double>(q), 1.5);
        EXPECT_NEAR(count / static_cast<double>(kDraws), expected, 0.03 * expected + 0.001) << q;
    }
    EXPECT_EQ(drawParentSize(random, 20, 0.01, 1'000), 1'000); // a tail this heavy hits the cap
}

TEST(ExecutionTest, TwapWorksAParentAtAnEvenPaceThenPauses) {
    ExecutionTrader trader{kTwap};
    FakeContext context;
    trader.onStart(context);
    ASSERT_EQ(context.wakeups.size(), 1U);
    EXPECT_GT(context.wakeups[0].first, 0); // the first parent waits for a pause too

    // Twelve lots: five, five and two, a second apart, each labelled with the parent.
    std::vector<NewOrder> children;
    std::vector<Timestamp> times;
    for (int i = 0; i < 3; ++i) {
        wake(trader, context);
        const NewOrder order = child(context);
        EXPECT_EQ(order.type, OrderType::Market);
        EXPECT_EQ(order.side, trader.side());
        EXPECT_EQ(order.parent, 1U);
        children.push_back(order);
        times.push_back(context.now());
    }
    std::vector<Quantity> sizes;
    for (const NewOrder& order : children) {
        sizes.push_back(order.quantity);
    }
    EXPECT_EQ(sizes, (std::vector<Quantity>{5, 5, 2}));
    EXPECT_EQ(times[2] - times[1], kSecond);
    EXPECT_EQ(times[1] - times[0], kSecond);

    // While a child is in flight the parent is not done: it could come back unfilled.
    fillAll(context, children[0]);
    fillAll(context, children[1]);
    wake(trader, context);
    EXPECT_TRUE(context.takeSent().empty());
    EXPECT_EQ(trader.parent(), 1U);

    // Once it fills the parent is done, and after a pause the next one gets the next id.
    fillAll(context, children[2]);
    wake(trader, context);
    EXPECT_TRUE(context.takeSent().empty());
    EXPECT_EQ(trader.parent(), 0U);
    wake(trader, context);
    EXPECT_EQ(trader.parent(), 2U);
    EXPECT_EQ(child(context).parent, 2U);
}

TEST(ExecutionTest, BuysAndSellsAboutEquallyOften) {
    ExecutionConfig single = kTwap;
    single.minParent = 5;
    single.maxParent = 5; // one child per parent
    ExecutionTrader trader{single};
    FakeContext context;
    trader.onStart(context);
    int buys = 0;
    for (int i = 0; i < 400; ++i) {
        wake(trader, context); // starts a parent and sends its child
        const NewOrder order = child(context);
        buys += order.side == Side::Buy ? 1 : 0;
        fillAll(context, order);
        wake(trader, context); // the parent is done
    }
    EXPECT_NEAR(buys, 200, 40);
}

TEST(ExecutionTest, SendsAgainWhatTheBookCouldNotFill) {
    ExecutionTrader trader{kTwap};
    FakeContext context;
    trader.onStart(context);
    wake(trader, context);
    const NewOrder first = child(context);
    context.accept(first.clientOrderId);
    context.fill(first.clientOrderId, 2);
    trader.onCancelled(context, context.cancelRest(first.clientOrderId)); // 3 lots unfilled
    EXPECT_EQ(trader.unsent(), 10);

    for (const Quantity expected : {5, 5}) {
        wake(trader, context);
        const NewOrder order = child(context);
        EXPECT_EQ(order.quantity, expected);
        fillAll(context, order);
    }
    wake(trader, context);
    EXPECT_EQ(trader.parent(), 0U); // twelve lots traded in all
}

TEST(ExecutionTest, GivesUpOnAParentWhenAChildIsRejected) {
    ExecutionTrader trader{kTwap};
    FakeContext context;
    trader.onStart(context);
    wake(trader, context);
    const NewOrder first = child(context);
    wake(trader, context);
    const NewOrder second = child(context);
    trader.onRejected(context, context.reject(second.clientOrderId, RejectReason::PositionLimit));
    // The first child's remainder, cancelled afterwards, is not sent again either.
    context.accept(first.clientOrderId);
    trader.onCancelled(context, context.cancelRest(first.clientOrderId));

    wake(trader, context);
    EXPECT_TRUE(context.takeSent().empty());
    EXPECT_EQ(trader.parent(), 0U);
    wake(trader, context);
    EXPECT_EQ(trader.parent(), 2U);
    EXPECT_EQ(trader.unsent() + child(context).quantity, 12);
}

TEST(ExecutionTest, PausesInAnAuctionAndCarriesOnAfter) {
    ExecutionTrader trader{kTwap};
    FakeContext context;
    context.snapshot.phase = Phase::OpeningAuction;
    trader.onStart(context);
    wake(trader, context);
    wake(trader, context);
    EXPECT_TRUE(context.takeSent().empty()); // a parent, but no children in the auction
    EXPECT_NE(trader.parent(), 0U);
    context.snapshot.phase = Phase::Continuous;
    wake(trader, context);
    EXPECT_EQ(child(context).quantity, 5);
}

TEST(ExecutionTest, AChildTurnedAwayByAnAuctionIsSentAgain) {
    ExecutionTrader trader{kTwap};
    FakeContext context;
    trader.onStart(context);
    wake(trader, context);
    const NewOrder first = child(context);
    fillAll(context, first);
    trader.onFilled(context, OrderFilled{.quantity = first.quantity});
    wake(trader, context);
    const NewOrder second = child(context);
    // The market went into a halt while the second child was on its way.
    trader.onRejected(context,
                      context.reject(second.clientOrderId, RejectReason::AuctionOrderType));
    EXPECT_EQ(trader.unsent(), 7); // twelve, less the five filled
    wake(trader, context);
    EXPECT_EQ(child(context).quantity, 5);
}

TEST(ExecutionTest, VwapPacesItsChildrenByTheDaysCurve) {
    ExecutionConfig config = kTwap;
    config.style = ExecutionStyle::Vwap;
    config.minParent = config.maxParent = 100;
    config.childSize = 10;
    config.activity = {.amplitude = 2.0, .day = 100 * kSecond};
    ExecutionTrader trader{config};
    FakeContext context;
    trader.onStart(context);
    context.setNow(0);
    trader.onWakeup(context, 0);
    EXPECT_EQ(child(context).quantity, 18); // 1.8 times the pace at the open
    context.setNow(50 * kSecond);
    trader.onWakeup(context, 0);
    EXPECT_EQ(child(context).quantity, 6); // and 0.6 times at midday
}

TEST(ExecutionTest, PovKeepsItsShareOfTheVolume) {
    ExecutionTrader trader{{.style = ExecutionStyle::Pov,
                            .minParent = 1'000,
                            .maxParent = 1'000,
                            .pause = kSecond,
                            .interval = kSecond,
                            .childSize = 100,
                            .participation = 0.25}};
    FakeContext context;
    context.snapshot.volume = 5'000;
    trader.onStart(context);
    wake(trader, context);
    EXPECT_TRUE(context.takeSent().empty()); // nothing has traded since it started

    // A quarter of everything traded since the start, its own lots included, at most 100 a time.
    const auto sendsAfter = [&](Quantity volume) {
        context.snapshot.volume = volume;
        wake(trader, context);
        const std::vector<Request> sent = context.takeSent();
        return sent.empty() ? Quantity{0} : std::get<NewOrder>(sent.front()).quantity;
    };
    EXPECT_EQ(sendsAfter(5'040), 10);
    EXPECT_EQ(sendsAfter(5'100), 15);
    EXPECT_EQ(sendsAfter(5'100), 0);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(sendsAfter(7'000), 100) << i; // 500 allowed in all: 475 to go, 100 at a time
    }
    EXPECT_EQ(sendsAfter(7'000), 75);
    EXPECT_EQ(sendsAfter(7'000), 0);
}

TEST(ExecutionTest, AgentsStartedTogetherSpreadOut) {
    const std::vector<Timestamp> first =
        test::firstWakeups([] { return std::make_unique<ExecutionTrader>(kTwap); }, 20);
    EXPECT_GT(std::set<Timestamp>(first.begin(), first.end()).size(), 15U);
}

TEST(ExecutionTest, ChildOrdersCarryTheirParentIntoTheLog) {
    const Scenario scenario = parseScenario(R"(
seed = 4
duration = "600s"
reference_price = 1000

[[agents]]
type = "zero_intelligence"
count = 50
limit_rate = 2.0
market_rate = 0.5

[[agents]]
type = "execution"
count = 3
min_parent = 10
max_parent = 200
pause = "20s"
child_size = 3
account = { max_position = 100000 }
)");
    std::ostringstream log;
    CsvEventLog sink{log, {"new"}};
    const RunResult result = runScenario(scenario, AgentRegistry::withBuiltIns(), &sink);

    // Every new order from the execution agents names its parent; each parent has one side, and
    // each agent numbers its parents 1, 2, 3, ...
    const std::set<AgentId> brokers(result.groups[1].agents.begin(),
                                    result.groups[1].agents.end());
    std::map<std::pair<AgentId, std::uint64_t>, std::set<std::string>> sides;
    std::string line;
    std::istringstream rows{log.str()};
    std::getline(rows, line); // the header
    while (std::getline(rows, line)) {
        std::vector<std::string> fields;
        std::istringstream split{line};
        for (std::string field; std::getline(split, field, ',');) {
            fields.push_back(field);
        }
        fields.resize(20);
        const auto agent = static_cast<AgentId>(std::stoul(fields[2]));
        if (!brokers.contains(agent)) {
            EXPECT_EQ(fields[19], "") << line;
            continue;
        }
        ASSERT_NE(fields[19], "") << line;
        sides[{agent, std::stoull(fields[19])}].insert(fields[5]);
    }
    std::map<AgentId, std::uint64_t> parents;
    for (const auto& [key, side] : sides) {
        EXPECT_EQ(side.size(), 1U);
        EXPECT_EQ(key.second, ++parents[key.first]); // no gaps in the numbering
    }
    for (const AgentId broker : brokers) {
        EXPECT_GT(parents[broker], 5U) << broker;
    }
    EXPECT_GT(result.groups[1].traded, 0);
}

TEST(ExecutionTest, RejectsInvalidConfigs) {
    const auto invalid = [](auto change) {
        ExecutionConfig config = kTwap;
        change(config);
        return config;
    };
    for (const ExecutionConfig& config :
         {invalid([](ExecutionConfig& c) { c.minParent = 0; }),
          invalid([](ExecutionConfig& c) { c.maxParent = 11; }),
          invalid([](ExecutionConfig& c) { c.maxParent = kMaxQuantity + 1; }),
          invalid([](ExecutionConfig& c) { c.parentTail = 0.0; }),
          invalid([](ExecutionConfig& c) {
              c.parentTail = std::numeric_limits<double>::infinity();
          }),
          invalid([](ExecutionConfig& c) { c.pause = -1; }),
          invalid([](ExecutionConfig& c) { c.interval = 0; }),
          invalid([](ExecutionConfig& c) { c.childSize = 0; }),
          invalid([](ExecutionConfig& c) { c.participation = 0.0; }),
          invalid([](ExecutionConfig& c) { c.participation = 1.0; })}) {
        EXPECT_THROW(ExecutionTrader{config}, std::invalid_argument);
    }
}

} // namespace
} // namespace crowdbook

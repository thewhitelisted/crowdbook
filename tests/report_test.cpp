#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/fundamental.hpp"
#include "crowdbook/report.hpp"
#include "crowdbook/scenario_file.hpp"
#include "crowdbook/session.hpp"
#include "exchange_test_support.hpp"

namespace crowdbook {
namespace {

using test::limitOrder;
using test::marketOrder;

// A busy market with a true value, informed traders and scoring.
constexpr std::string_view kBusy = R"(seed = 4
duration = "40s"
reference_price = 1000

[scoring]
mark = "value"
inventory_penalty = 0.01

[fundamental]
volatility = 1.0
step = "250ms"

[exchange]
depth_levels = 3
maker_fee = -0.1
taker_fee = 0.3

[participant]
latency = { to_exchange = "1ms", from_exchange = "1ms" }
account = { max_position = 50, max_order_quantity = 10 }

[[agents]]
type = "zero_intelligence"
name = "noise"
count = 20
limit_rate = 2.0
market_rate = 0.5

[[agents]]
type = "market_maker"
name = "maker"

[[agents]]
type = "informed"
name = "informed"
count = 3
interval = "500ms"
order_size = 2
)";

// A market where nothing trades unless the seats do: the one trend follower's threshold is out
// of reach.
constexpr std::string_view kQuiet = R"(duration = "10s"
reference_price = 1000

[participant]
latency = { to_exchange = "1ms", from_exchange = "1ms" }

[[agents]]
type = "momentum"
threshold = 1000000.0
)";

// Records a session of `seats` in `scenario`, each request sent at its time from its seat.
Session record(std::string_view scenario, const std::vector<std::string>& seats,
               const std::vector<SessionAction>& script, Timestamp end) {
    const Scenario parsed = parseScenario(scenario);
    SessionMarket market = openSession(parsed, AgentRegistry::withBuiltIns(), nullptr, seats);
    Session session{.scenario = std::string{scenario}, .seed = parsed.seed, .seats = seats};
    for (SessionAction action : script) {
        market.run.runUntil(action.time);
        const ClientOrderId id =
            perform(market.run.simulation(), market.seats[action.seat].agent, action);
        if (auto* order = std::get_if<NewOrder>(&action.request)) {
            order->clientOrderId = id;
        }
        session.actions.push_back(action);
    }
    market.run.runUntil(end);
    session.end = end;
    return session;
}

TEST(ReportTest, NamesWhoEachFillWasWith) {
    const Session session =
        record(kQuiet, {"alice", "bob"},
               {{.time = kSecond, .seat = 0, .request = limitOrder(0, Side::Buy, 1'001, 4)},
                {.time = 2 * kSecond, .seat = 1, .request = marketOrder(0, Side::Sell, 3)}},
               5 * kSecond);
    const SessionReport report = makeReport(session, AgentRegistry::withBuiltIns());
    ASSERT_EQ(report.seats.size(), 2U);
    const SeatReport& alice = report.seats[0];
    ASSERT_EQ(alice.fills.size(), 1U);
    EXPECT_EQ(alice.fills[0].group, "bob");
    EXPECT_EQ(alice.fills[0].type, "participant");
    EXPECT_EQ(alice.fills[0].liquidity, Liquidity::Maker);
    EXPECT_EQ(alice.fills[0].quantity, 3);
    EXPECT_EQ(alice.fills[0].time, 2 * kSecond + kMillisecond);
    ASSERT_EQ(alice.counterparties.size(), 1U);
    EXPECT_EQ(alice.counterparties[0].bought, 3);
    const SeatReport& bob = report.seats[1];
    ASSERT_EQ(bob.fills.size(), 1U);
    EXPECT_EQ(bob.fills[0].group, "alice");
    EXPECT_EQ(bob.fills[0].side, Side::Sell);
    // Without a true value there is no edge, and a quiet book has no mid to mark out against.
    EXPECT_FALSE(alice.counterparties[0].edge);
    EXPECT_FALSE(alice.fills[0].value);
}

// The quiet market with a true value that stays at 1000.
constexpr std::string_view kQuietValue = R"(duration = "10s"
reference_price = 1000

[fundamental]
volatility = 0.0

[participant]
latency = { to_exchange = "1ms", from_exchange = "1ms" }

[[agents]]
type = "momentum"
threshold = 1000000.0
)";

TEST(ReportTest, MarksFillsOutAgainstTheMidAfterThemAndTheValue) {
    // Alice quotes 1001 bid, 1005 offered; Bob sells her a lot at 2s, when the mid is 1003; Alice
    // bids 1003 at 5s, which makes the mid 1004.
    const Session session =
        record(kQuietValue, {"alice", "bob"},
               {{.time = kSecond, .seat = 0, .request = limitOrder(0, Side::Buy, 1'001, 4)},
                {.time = kSecond, .seat = 0, .request = limitOrder(0, Side::Sell, 1'005, 4)},
                {.time = 2 * kSecond, .seat = 1, .request = marketOrder(0, Side::Sell, 1)},
                {.time = 5 * kSecond, .seat = 0, .request = limitOrder(0, Side::Buy, 1'003, 1)}},
               15 * kSecond);
    const SessionReport report = makeReport(session, AgentRegistry::withBuiltIns());
    const SeatReport& alice = report.seats[0];
    ASSERT_EQ(alice.fills.size(), 1U);
    EXPECT_EQ(alice.fills[0].midAfter[0], 1'003.0);
    EXPECT_EQ(alice.fills[0].midAfter[1], 1'004.0);
    EXPECT_FALSE(alice.fills[0].midAfter[2]); // the session ends before a minute is up
    EXPECT_EQ(alice.fills[0].value, 1'000.0);
    ASSERT_EQ(alice.counterparties.size(), 1U);
    EXPECT_EQ(alice.counterparties[0].markout, 3.0); // bought at 1001, the mid 1004 ten seconds on
    // Bob sold at 1001 what was worth 1000: a tick of edge against Alice.
    EXPECT_EQ(alice.counterparties[0].edge, 1.0);
    const SeatReport& bob = report.seats[1];
    EXPECT_EQ(bob.counterparties[0].markout, -3.0);
    EXPECT_EQ(bob.counterparties[0].edge, -1.0);
}

TEST(ReportTest, AgreesWithTheSessionsResults) {
    std::vector<SessionAction> script;
    for (int i = 0; i < 30; ++i) {
        const Timestamp time = kSecond + i * 900 * kMillisecond;
        script.push_back({.time = time,
                          .request = i % 3 == 0 ? marketOrder(0, i % 2 == 0 ? Side::Buy
                                                                             : Side::Sell,
                                                              2)
                                                : limitOrder(0, i % 2 == 0 ? Side::Buy
                                                                           : Side::Sell,
                                                             i % 2 == 0 ? 999 : 1'001, 3)});
    }
    const Session session = record(kBusy, {"you"}, script, 40 * kSecond);
    const RunResult result = replaySession(session, AgentRegistry::withBuiltIns());
    const SessionReport report = makeReport(session, AgentRegistry::withBuiltIns());
    ASSERT_EQ(report.seats.size(), 1U);
    const SeatReport& seat = report.seats[0];
    const GroupResult& you = result.groups.back();

    Quantity traded = 0;
    for (const ReportFill& fill : seat.fills) {
        traded += fill.quantity;
        ASSERT_TRUE(fill.value);
    }
    EXPECT_EQ(traded, you.traded);
    EXPECT_GT(traded, 0);
    ASSERT_EQ(seat.pnl.size(), 41U); // every second from 0 to 40
    EXPECT_EQ(seat.pnl.back().position, you.position);
    EXPECT_EQ(seat.pnl.back().cash, you.cash);
    EXPECT_DOUBLE_EQ(seat.pnl.back().pnl, static_cast<double>(you.pnl) -
                                              static_cast<double>(you.fees) / 1'000.0);
    ASSERT_TRUE(seat.pnl.back().pnlAtValue);
    EXPECT_DOUBLE_EQ(*seat.pnl.back().pnlAtValue,
                     static_cast<double>(you.cash) +
                         static_cast<double>(you.position) * *result.finalValue -
                         static_cast<double>(you.fees) / 1'000.0);
    ASSERT_TRUE(seat.score);
    EXPECT_EQ(*seat.score, result.scores[0].score);
    EXPECT_EQ(report.finalValue, result.finalValue);

    // The true values are the market's: an independent copy of the value's path agrees.
    const Scenario scenario = parseScenario(kBusy);
    Fundamental value{*scenario.fundamental, Random{scenario.seed, 0}};
    for (const ReportFill& fill : seat.fills) {
        EXPECT_DOUBLE_EQ(*fill.value, value.valueAt(fill.time));
    }
    // The counterparties account for every fill and every lot.
    std::int64_t fills = 0;
    Quantity lots = 0;
    for (const Counterparty& party : seat.counterparties) {
        fills += party.fills;
        lots += party.bought + party.sold;
    }
    EXPECT_EQ(fills, static_cast<std::int64_t>(seat.fills.size()));
    EXPECT_EQ(lots, traded);
}

TEST(ReportTest, ASeatThatNeverTradesMovesNothing) {
    const Session session = record(kBusy, {"you"}, {}, 20 * kSecond);
    const SessionReport report = makeReport(session, AgentRegistry::withBuiltIns());
    const SeatReport& seat = report.seats[0];
    EXPECT_TRUE(seat.fills.empty());
    EXPECT_EQ(seat.lastPrice, seat.lastPriceWithout);
    ASSERT_TRUE(seat.impact);
    EXPECT_EQ(*seat.impact, 0.0);
    for (const PricePoint& point : seat.prices) {
        EXPECT_EQ(point.mid, point.midWithout);
    }
}

TEST(ReportTest, ASeatThatTradesMovesTheMarket) {
    std::vector<SessionAction> script;
    for (int i = 0; i < 10; ++i) {
        script.push_back({.time = kSecond + i * 500 * kMillisecond,
                          .request = marketOrder(0, Side::Buy, 10)});
    }
    const Session session = record(kBusy, {"you"}, script, 20 * kSecond);
    const SeatReport seat = makeReport(session, AgentRegistry::withBuiltIns()).seats[0];
    ASSERT_TRUE(seat.impact);
    EXPECT_GT(*seat.impact, 0.0); // buying a hundred lots pushes the price up
}

TEST(ReportTest, TheJsonIsTheSameEveryTime) {
    const Session session =
        record(kBusy, {"you"},
               {{.time = kSecond, .request = marketOrder(0, Side::Buy, 5)}}, 10 * kSecond);
    std::ostringstream first;
    std::ostringstream second;
    writeReportJson(first, makeReport(session, AgentRegistry::withBuiltIns()));
    writeReportJson(second, makeReport(session, AgentRegistry::withBuiltIns()));
    EXPECT_EQ(first.str(), second.str());
    EXPECT_NE(first.str().find("\"group\": \""), std::string::npos);
    EXPECT_THROW(static_cast<void>(makeReport(session, AgentRegistry::withBuiltIns(), 0)),
                 std::invalid_argument);
}

} // namespace
} // namespace crowdbook

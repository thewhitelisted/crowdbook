#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/gateway.hpp"
#include "crowdbook/protocol.hpp"
#include "crowdbook/scenario_file.hpp"
#include "crowdbook/session.hpp"

namespace crowdbook {
namespace {

// A short day: a 10 s opening auction, continuous trading, a 10 s closing auction.
constexpr std::string_view kDay = R"(seed = 8
duration = "60s"
reference_price = 1000

[trading_day]
opening_auction = "10s"
closing_auction = "10s"
activity = 2.0

[exchange]
depth_levels = 3
auction_fee = 0.05

[participant]
latency = { to_exchange = "1ms", from_exchange = "1ms" }

[[agents]]
type = "zero_intelligence"
name = "noise"
count = 30
limit_rate = 2.0
market_rate = 0.5

[[agents]]
type = "market_maker"
name = "maker"

[[agents]]
type = "execution"
name = "brokers"
count = 2
style = "vwap"
pause = "1s"
interval = "200ms"
)";

struct Row {
    Timestamp time = 0;
    std::string kind;
    std::string price;
    std::string liquidity;
    std::string reason;
};

// The event log's rows, with the columns these tests look at.
std::vector<Row> rows(const std::string& log) {
    std::vector<Row> result;
    std::istringstream lines{log};
    std::string line;
    std::getline(lines, line); // the header
    while (std::getline(lines, line)) {
        std::vector<std::string> fields;
        std::istringstream columns{line};
        for (std::string field; std::getline(columns, field, ',');) {
            fields.push_back(field);
        }
        fields.resize(20);
        result.push_back({.time = std::stoll(fields[0]),
                          .kind = fields[1],
                          .price = fields[8],
                          .liquidity = fields[11],
                          .reason = fields[14]});
    }
    return result;
}

std::string runLog(const Scenario& scenario) {
    std::ostringstream log;
    CsvEventLog sink{log};
    static_cast<void>(runScenario(scenario, AgentRegistry::withBuiltIns(), &sink));
    return log.str();
}

TEST(TradingDayTest, TheDayGoesThroughItsPhasesOnSchedule) {
    const std::vector<Row> log = rows(runLog(parseScenario(kDay)));
    std::vector<Row> phases;
    for (const Row& row : log) {
        if (row.kind == "phase") {
            phases.push_back(row);
        }
    }
    ASSERT_EQ(phases.size(), 4U);
    EXPECT_EQ(phases[0].time, 0);
    EXPECT_EQ(phases[0].reason, "opening auction");
    EXPECT_EQ(phases[1].time, 10 * kSecond);
    EXPECT_EQ(phases[1].reason, "continuous");
    EXPECT_FALSE(phases[1].price.empty()); // the opening auction traded
    EXPECT_EQ(phases[2].time, 50 * kSecond);
    EXPECT_EQ(phases[2].reason, "closing auction");
    EXPECT_EQ(phases[3].time, 60 * kSecond);
    EXPECT_EQ(phases[3].reason, "closed");
    EXPECT_FALSE(phases[3].price.empty()); // and so did the closing auction

    // In the auctions, trades happen only at the uncross, all at its price.
    int continuous = 0;
    for (const Row& row : log) {
        if (row.kind != "trade") {
            continue;
        }
        if (row.time == 10 * kSecond && row.liquidity == "auction") {
            EXPECT_EQ(row.price, phases[1].price);
        } else if (row.time == 60 * kSecond) {
            EXPECT_EQ(row.liquidity, "auction");
            EXPECT_EQ(row.price, phases[3].price);
        } else {
            EXPECT_GT(row.time, 10 * kSecond);
            EXPECT_LT(row.time, 50 * kSecond);
            EXPECT_EQ(row.liquidity, "");
            ++continuous;
        }
    }
    EXPECT_GT(continuous, 100);
}

TEST(TradingDayTest, TheLastPriceIsTheClose) {
    Scenario scenario = parseScenario(kDay);
    const RunResult result = runScenario(scenario, AgentRegistry::withBuiltIns());
    std::string close;
    for (const Row& row : rows(runLog(scenario))) {
        if (row.kind == "phase" && row.reason == "closed") {
            close = row.price;
        }
    }
    EXPECT_EQ(std::to_string(result.lastPrice), close);
}

TEST(TradingDayTest, AHaltLastsItsLengthUnlessTheCloseComesFirst) {
    Scenario scenario = parseScenario(kDay);
    scenario.tradingDay->haltBand = 2;
    scenario.tradingDay->halt = 3 * kSecond;
    const std::vector<Row> log = rows(runLog(scenario));
    int halts = 0;
    Timestamp haltedAt = -1;
    for (const Row& row : log) {
        if (row.kind != "phase") {
            continue;
        }
        if (row.reason == "halt") {
            ++halts;
            haltedAt = row.time;
        } else if (haltedAt >= 0) {
            if (row.reason == "continuous") {
                EXPECT_EQ(row.time, haltedAt + 3 * kSecond);
            } else {
                EXPECT_EQ(row.reason, "closing auction"); // the close came first
                EXPECT_LT(row.time, haltedAt + 3 * kSecond);
            }
            haltedAt = -1;
        }
    }
    EXPECT_GT(halts, 1);
}

TEST(TradingDayTest, NothingButCancelsAfterTheClose) {
    Scenario scenario = parseScenario(kDay);
    SessionMarket market = openSession(scenario, AgentRegistry::withBuiltIns());
    market.run.runUntil(61 * kSecond);
    Simulation& simulation = market.run.simulation();
    EXPECT_EQ(simulation.exchange().phase(), Phase::Closed);
    static_cast<void>(perform(simulation, market.seats[0].agent,
                              {.time = simulation.now(),
                               .request = NewOrder{.side = Side::Buy,
                                                   .type = OrderType::Limit,
                                                   .price = 1'000,
                                                   .quantity = 1}}));
    market.run.runUntil(62 * kSecond);
    EXPECT_EQ(market.seats[0].participant->lastRejection()->reason, RejectReason::MarketClosed);
}

// A served trading day, with a duration of its own, replays exactly: the session records the
// duration, which the day's schedule depends on.
TEST(TradingDayTest, AServedDayWithItsOwnDurationReplaysExactly) {
    Scenario scenario = parseScenario(kDay);
    scenario.duration = 40 * kSecond;
    std::ostringstream live;
    CsvEventLog sink{live};
    Gateway gateway{scenario, std::string{kDay}, AgentRegistry::withBuiltIns(), {}, &sink};
    const ConnectionId client = gateway.connect(0);
    gateway.receive(client, protocol::encode(protocol::Hello{.seat = "you"}), 0);
    // A limit order in the opening auction, which takes part in the uncross, and a market
    // order in it, which is turned away.
    gateway.receive(client,
                    protocol::encode(NewOrder{.clientOrderId = 1,
                                              .side = Side::Buy,
                                              .type = OrderType::Limit,
                                              .price = 1'010,
                                              .quantity = 3}),
                    2 * kSecond);
    gateway.receive(client,
                    protocol::encode(NewOrder{.clientOrderId = 2,
                                              .side = Side::Buy,
                                              .type = OrderType::Market,
                                              .quantity = 3}),
                    2 * kSecond);
    for (std::int64_t wall = 0; wall <= 41 * kSecond; wall += 100 * kMillisecond) {
        gateway.advance(wall);
        gateway.consumeOutput(client, gateway.pendingOutput(client).size());
    }
    ASSERT_TRUE(gateway.finished());
    EXPECT_EQ(gateway.session().duration, 40 * kSecond);
    std::ostringstream file;
    writeSession(file, gateway.session());
    const Session read = parseSession(file.str());
    EXPECT_EQ(read.duration, 40 * kSecond);
    std::ostringstream replayed;
    CsvEventLog replaySink{replayed};
    static_cast<void>(replaySession(read, AgentRegistry::withBuiltIns(), &replaySink));
    EXPECT_EQ(replayed.str(), live.str());
    // The closing auction came at 30 s, ten seconds before the session's own end.
    EXPECT_NE(live.str().find("30000000000,phase,,,,,,,,,,,,,closing auction"),
              std::string::npos);
}

TEST(TradingDayTest, ScenarioFilesDescribeTheDay) {
    const Scenario scenario = parseScenario(kDay);
    ASSERT_TRUE(scenario.tradingDay);
    EXPECT_EQ(scenario.tradingDay->openingAuction, 10 * kSecond);
    EXPECT_EQ(scenario.tradingDay->activity, 2.0);
    EXPECT_EQ(scenario.exchange.auctionFee, 50);
    const auto error = [](std::string_view text) {
        try {
            static_cast<void>(parseScenario(text));
        } catch (const ScenarioError& thrown) {
            return std::string{thrown.what()};
        }
        return std::string{};
    };
    const std::string agent = "[[agents]]\ntype = \"momentum\"\n";
    EXPECT_NE(error("duration = \"10s\"\n[trading_day]\nopening_auction = \"8s\"\n"
                    "closing_auction = \"8s\"\n" + agent)
                  .find("must fit in the duration"),
              std::string::npos);
    EXPECT_NE(error("[trading_day]\nhalt_band = 5\n" + agent).find("needs a halt"),
              std::string::npos);
    EXPECT_NE(error("[trading_day]\nactivity = -1\n" + agent).find("must not be negative"),
              std::string::npos);
    EXPECT_NE(error("[exchange]\nauction_fee = -0.1\ntaker_fee = 0.2\n" + agent)
                  .find("'auction_fee' must not be negative"),
              std::string::npos);
    EXPECT_NE(error("[trading_day]\nlunch = \"1h\"\n" + agent).find("unknown key 'lunch'"),
              std::string::npos);
}

} // namespace
} // namespace crowdbook

#include "crowdbook/exchange.hpp"
#include "exchange_test_support.hpp"

namespace crowdbook {
namespace {

using test::limitOrder;
using test::marketOrder;

// An exchange with three traders, a reference price of 100 and a halt band of 5.
struct DayExchange {
    DayExchange() {
        for (const AgentId agent : {1U, 2U, 3U}) {
            exchange.addAgent(agent);
        }
    }

    std::vector<Event> send(AgentId agent, const Request& request) {
        std::vector<Event> events;
        exchange.handle(agent, request, events);
        return events;
    }

    std::vector<Event> phase(Phase next) {
        std::vector<Event> events;
        exchange.setPhase(next, events);
        return events;
    }

    Exchange exchange{{.referencePrice = 100, .haltBand = 5, .haltDuration = kSecond}};
};

bool halted(const std::vector<Event>& events) {
    return std::ranges::any_of(events, [](const Event& event) {
        const auto* phase = std::get_if<PhaseChanged>(&event);
        return phase != nullptr && phase->phase == Phase::HaltAuction;
    });
}

TEST(TradingDayExchangeTest, AnAuctionAnnouncesWhereItStandsAtOnce) {
    DayExchange day;
    const std::vector<Event> events = day.phase(Phase::OpeningAuction);
    // Nothing would trade in an empty book, and it says so.
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(std::get<PhaseChanged>(events.front()).phase, Phase::OpeningAuction);
    EXPECT_EQ(events.back(), Event{Indicative{}});
}

TEST(TradingDayExchangeTest, HaltsAreMeasuredFromTheLastAuctionsPrice) {
    DayExchange day;
    day.phase(Phase::OpeningAuction);
    day.send(1, limitOrder(1, Side::Buy, 120, 5));
    day.send(2, limitOrder(1, Side::Sell, 120, 5));
    const std::vector<Event> open = day.phase(Phase::Continuous);
    EXPECT_EQ(std::get<PhaseChanged>(*std::ranges::find_if(open, [](const Event& event) {
                  return std::holds_alternative<PhaseChanged>(event);
              })).price,
              120);
    EXPECT_EQ(day.exchange.reference(), 120);

    // 125 is within five ticks of the open at 120, though twenty-five from the reference price.
    day.send(1, limitOrder(2, Side::Sell, 125, 1));
    EXPECT_FALSE(halted(day.send(2, limitOrder(2, Side::Buy, 125, 1))));
    EXPECT_EQ(day.exchange.phase(), Phase::Continuous);
    // 126 is not.
    day.send(1, limitOrder(3, Side::Sell, 126, 1));
    EXPECT_TRUE(halted(day.send(2, marketOrder(3, Side::Buy, 1))));
    EXPECT_EQ(day.exchange.phase(), Phase::HaltAuction);
    // And the halt's auction takes no market orders.
    const std::vector<Event> refused = day.send(3, marketOrder(1, Side::Buy, 1));
    EXPECT_EQ(std::get<OrderRejected>(refused.front()).reason, RejectReason::AuctionOrderType);
}

} // namespace
} // namespace crowdbook

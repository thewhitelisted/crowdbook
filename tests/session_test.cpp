#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/scenario_file.hpp"
#include "crowdbook/session.hpp"
#include "exchange_test_support.hpp"

namespace crowdbook {
namespace {

using test::limitOrder;
using test::marketOrder;

// A busy little market with a depth feed and a participant. The comment has characters a session
// file has to escape.
constexpr std::string_view kScenario = R"(# a "quoted" \ comment	with a tab
seed = 3
duration = "10s"
reference_price = 1000

[exchange]
depth_levels = 5

[participant]
latency = { to_exchange = "1ms", from_exchange = "1ms" }
account = { max_position = 50, max_order_quantity = 10 }

[[agents]]
type = "zero_intelligence"
count = 10
limit_rate = 5.0
market_rate = 1.0

[[agents]]
type = "market_maker"
)";

// Plays a session the way a live one goes: the market runs in uneven steps, and each action is
// performed at the time it happens and recorded with the id it was given. Returns the session and
// the event log written along the way.
std::pair<Session, std::string> playScripted() {
    std::ostringstream log;
    CsvEventLog sink{log};
    const Scenario scenario = parseScenario(kScenario);
    SessionMarket market = openSession(scenario, AgentRegistry::withBuiltIns(), &sink);
    Session session{.scenario = std::string{kScenario}, .seed = scenario.seed};
    const auto act = [&](Timestamp time, Request request) {
        market.run.runUntil(time);
        SessionAction action{.time = time, .request = std::move(request)};
        const ClientOrderId id = perform(market.run.simulation(), market.participant, action);
        if (auto* order = std::get_if<NewOrder>(&action.request)) {
            order->clientOrderId = id;
        }
        session.actions.push_back(action);
    };

    market.run.runUntil(300 * kMillisecond);
    market.run.runUntil(700 * kMillisecond);
    act(kSecond, limitOrder(0, Side::Buy, 999, 5));
    NewOrder part = marketOrder(0, Side::Sell, 3);
    part.parent = 4; // part of a larger order, which the file has to keep
    act(kSecond, part);
    act(2 * kSecond + 17, ModifyOrder{.clientOrderId = 1, .price = 1'000, .quantity = 4});
    act(3 * kSecond, limitOrder(0, Side::Sell, 1'003, 2, TimeInForce::PostOnly));
    act(4 * kSecond, CancelOrder{.clientOrderId = 1});
    market.run.runUntil(5'500 * kMillisecond);
    market.run.runUntil(7 * kSecond);
    session.end = 7 * kSecond;
    return {session, log.str()};
}

TEST(SessionTest, ReplaysToTheSameLogByteForByte) {
    const auto [session, live] = playScripted();
    std::ostringstream file;
    writeSession(file, session);
    const Session read = parseSession(file.str());
    EXPECT_EQ(read, session);

    std::ostringstream replayed;
    CsvEventLog sink{replayed};
    const RunResult result = replaySession(read, AgentRegistry::withBuiltIns(), &sink);
    EXPECT_EQ(replayed.str(), live);
    EXPECT_GT(std::ranges::count(live, '\n'), 500); // a busy market, not an empty one
    ASSERT_EQ(result.groups.back().name, kParticipantGroup);
    EXPECT_GT(result.groups.back().traded, 0);
}

TEST(SessionTest, TheParticipantHasTheScenariosLatencyAndAccount) {
    SessionMarket market = openSession(parseScenario(kScenario), AgentRegistry::withBuiltIns());
    Simulation& simulation = market.run.simulation();
    market.run.runUntil(kSecond);
    static_cast<void>(perform(simulation, market.participant,
                              {.time = kSecond, .request = limitOrder(0, Side::Buy, 900, 10)}));
    static_cast<void>(perform(simulation, market.participant,
                              {.time = kSecond, .request = limitOrder(0, Side::Buy, 900, 11)}));

    // A millisecond there and a millisecond back.
    market.run.runUntil(kSecond + 2 * kMillisecond - 1);
    EXPECT_FALSE(simulation.ledger(market.participant).find(1)->acknowledged);
    market.run.runUntil(kSecond + 2 * kMillisecond);
    EXPECT_TRUE(simulation.ledger(market.participant).find(1)->acknowledged);
    // Eleven lots is over its largest order of ten.
    ASSERT_TRUE(market.agent->lastRejection().has_value());
    EXPECT_EQ(market.agent->lastRejection()->reason, RejectReason::OrderSizeLimit);
}

TEST(SessionTest, AReplayThatDivergesFailsLoudly) {
    Session session = playScripted().first;
    std::get<NewOrder>(session.actions[1].request).clientOrderId = 7;
    EXPECT_THROW(static_cast<void>(replaySession(session, AgentRegistry::withBuiltIns())),
                 std::logic_error);
}

// The message of the ScenarioError that parsing `text` throws, or "" if it parses.
std::string parseError(const std::string& text) {
    try {
        static_cast<void>(parseSession(text, "test.session"));
    } catch (const ScenarioError& error) {
        return error.what();
    }
    return "";
}

TEST(SessionTest, ReportsMalformedSessionsWithTheirLine) {
    const std::string scenario = "scenario = \"[[agents]]\\ntype = \\\"momentum\\\"\"\n";
    const std::string header = "session_version = 1\nseed = 1\nend_ns = 100\n" + scenario;
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"session_version = 2\nseed = 1\nend_ns = 100\n" + scenario, "test.session:1:"},
        {"session_version = 1\nseed = 1\n" + scenario, "missing 'end_ns'"},
        {header + "actions = [{ time_ns = 5, instrument = 0, request = \"buy\", "
                  "client_order_id = 1 }]\n",
         "'request' must be new, cancel or modify"},
        {header + "actions = [\n{ time_ns = 9, instrument = 0, request = \"cancel\", "
                  "client_order_id = 1 },\n{ time_ns = 5, instrument = 0, request = \"cancel\", "
                  "client_order_id = 1 }]\n",
         "test.session:7: actions must be in time order"},
        {header + "actions = [{ time_ns = 5, instrument = 1, request = \"cancel\", "
                  "client_order_id = 1 }]\n",
         "'instrument' must be a whole number between 0 and 0"},
        {"session_version = 1\nseed = 1\nend_ns = 100\nscenario = \"seed = 1\"\n",
         "the scenario has no [[agents]]"},
    };
    for (const auto& [text, expected] : cases) {
        EXPECT_NE(parseError(text).find(expected), std::string::npos)
            << "for:\n" << text << "got: " << parseError(text);
    }
}

} // namespace
} // namespace crowdbook

#include <algorithm>
#include <format>
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
        const ClientOrderId id =
            perform(market.run.simulation(), market.seats.front().agent, action);
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
    const Seat& seat = market.seats.front();
    market.run.runUntil(kSecond);
    static_cast<void>(perform(simulation, seat.agent,
                              {.time = kSecond, .request = limitOrder(0, Side::Buy, 900, 10)}));
    static_cast<void>(perform(simulation, seat.agent,
                              {.time = kSecond, .request = limitOrder(0, Side::Buy, 900, 11)}));

    // A millisecond there and a millisecond back.
    market.run.runUntil(kSecond + 2 * kMillisecond - 1);
    EXPECT_FALSE(simulation.ledger(seat.agent).find(1)->acknowledged);
    market.run.runUntil(kSecond + 2 * kMillisecond);
    EXPECT_TRUE(simulation.ledger(seat.agent).find(1)->acknowledged);
    // Eleven lots is over its largest order of ten.
    ASSERT_TRUE(seat.participant->lastRejection().has_value());
    EXPECT_EQ(seat.participant->lastRejection()->reason, RejectReason::OrderSizeLimit);
}

TEST(SessionTest, SeveralSeatsReplayToTheSameLogByteForByte) {
    std::ostringstream live;
    CsvEventLog sink{live};
    const Scenario scenario = parseScenario(kScenario);
    const std::vector<std::string> seats{"alice", "bob"};
    SessionMarket market = openSession(scenario, AgentRegistry::withBuiltIns(), &sink, seats);
    ASSERT_EQ(market.seats.size(), 2U);
    EXPECT_EQ(market.seats[0].name, "alice");
    EXPECT_EQ(market.seats[1].agent, market.seats[0].agent + 1);
    Session session{.scenario = std::string{kScenario}, .seed = scenario.seed, .seats = seats};
    const auto act = [&](Timestamp time, std::uint32_t seat, Request request) {
        market.run.runUntil(time);
        SessionAction action{.time = time, .seat = seat, .request = std::move(request)};
        const ClientOrderId id =
            perform(market.run.simulation(), market.seats[seat].agent, action);
        if (auto* order = std::get_if<NewOrder>(&action.request)) {
            order->clientOrderId = id;
        }
        session.actions.push_back(action);
    };
    // The two seats trade with each other at the same nanosecond, and each has its own ids.
    act(kSecond, 0, limitOrder(0, Side::Buy, 1'001, 4));
    act(kSecond, 1, limitOrder(0, Side::Sell, 1'001, 4));
    act(2 * kSecond, 1, limitOrder(0, Side::Sell, 1'010, 2));
    act(3 * kSecond, 1, CancelOrder{.clientOrderId = 2});
    market.run.runUntil(4 * kSecond);
    session.end = 4 * kSecond;

    std::ostringstream file;
    writeSession(file, session);
    const Session read = parseSession(file.str());
    EXPECT_EQ(read, session);
    std::ostringstream replayed;
    CsvEventLog replaySink{replayed};
    const RunResult result = replaySession(read, AgentRegistry::withBuiltIns(), &replaySink);
    EXPECT_EQ(replayed.str(), live.str());
    ASSERT_GE(result.groups.size(), 2U);
    EXPECT_EQ(result.groups[result.groups.size() - 2].name, "alice");
    EXPECT_EQ(result.groups.back().name, "bob");
}

// The market without a seat is the market where nobody sat there: replaying a session without
// its seat's orders gives the log of the scenario run with no participant, byte for byte.
TEST(SessionTest, AReplayWithoutTheSeatIsTheMarketWithoutAParticipant) {
    const auto [session, live] = playScripted();
    std::ostringstream without;
    CsvEventLog withoutSink{without};
    static_cast<void>(
        replaySession(session, AgentRegistry::withBuiltIns(), &withoutSink, {0}));
    Scenario scenario = parseScenario(kScenario);
    scenario.duration = session.end;
    std::ostringstream alone;
    CsvEventLog aloneSink{alone};
    static_cast<void>(runScenario(scenario, AgentRegistry::withBuiltIns(), &aloneSink));
    EXPECT_EQ(without.str(), alone.str());
    EXPECT_NE(without.str(), live); // the seat did change the market
}

// Rewinding to a moment and playing on with nothing new gives the session's own log up to that
// moment; playing on differently makes a new session that replays exactly.
TEST(SessionTest, ARewoundSessionPlaysOnFromTheMoment) {
    const auto [session, live] = playScripted();
    std::ostringstream log;
    CsvEventLog sink{log};
    RewoundSession rewound =
        rewindSession(session, 2'500 * kMillisecond, AgentRegistry::withBuiltIns(), &sink);
    ASSERT_EQ(rewound.record.actions.size(), 3U); // two orders at 1s and the modify at 2s + 17
    EXPECT_EQ(rewound.market.run.simulation().now(), 2'500 * kMillisecond);
    // Up to there, the log is the session's.
    EXPECT_TRUE(live.starts_with(log.str()));

    // Play on: a different order instead of the session's post-only offer at 3s.
    Simulation& simulation = rewound.market.run.simulation();
    rewound.market.run.runUntil(3 * kSecond);
    SessionAction action{.time = 3 * kSecond, .request = marketOrder(0, Side::Buy, 2)};
    std::get<NewOrder>(action.request).clientOrderId =
        perform(simulation, rewound.market.seats[0].agent, action);
    rewound.record.actions.push_back(action);
    rewound.market.run.runUntil(5 * kSecond);
    rewound.record.end = 5 * kSecond;
    EXPECT_NE(log.str(), live.substr(0, log.str().size()));

    std::ostringstream replayed;
    CsvEventLog replaySink{replayed};
    static_cast<void>(replaySession(rewound.record, AgentRegistry::withBuiltIns(), &replaySink));
    EXPECT_EQ(replayed.str(), log.str());
    EXPECT_THROW(static_cast<void>(rewindSession(session, session.end + 1,
                                                 AgentRegistry::withBuiltIns())),
                 ScenarioError);
}

TEST(SessionTest, SeatsAreCheckedWhenTheMarketOpens) {
    const Scenario scenario = parseScenario(kScenario);
    const AgentRegistry registry = AgentRegistry::withBuiltIns();
    for (const std::vector<std::string>& seats :
         {std::vector<std::string>{}, std::vector<std::string>{"a", "a"},
          std::vector<std::string>{"market_maker"}}) {
        EXPECT_THROW(static_cast<void>(openSession(scenario, registry, nullptr, seats)),
                     ScenarioError);
    }
}

TEST(SessionTest, VersionOneSessionsHaveOneSeat) {
    const std::string scenario = "scenario = \"[[agents]]\\ntype = \\\"momentum\\\"\"\n";
    const Session session = parseSession(
        "session_version = 1\nseed = 1\nend_ns = 100\n" + scenario +
        "actions = [{ time_ns = 5, instrument = 0, request = \"cancel\", client_order_id = 1 }]\n");
    EXPECT_EQ(session.seats, std::vector<std::string>{std::string{kParticipantGroup}});
    ASSERT_EQ(session.actions.size(), 1U);
    EXPECT_EQ(session.actions[0].seat, 0U);
}

TEST(SessionTest, AReplayThatDivergesFailsLoudly) {
    Session session = playScripted().first;
    std::get<NewOrder>(session.actions[1].request).clientOrderId = 7;
    EXPECT_THROW(static_cast<void>(replaySession(session, AgentRegistry::withBuiltIns())),
                 std::logic_error);
    // A session from another crowdbook says so, since that is the likely reason.
    session.recordedBy = "0.1.0";
    try {
        static_cast<void>(replaySession(session, AgentRegistry::withBuiltIns()));
        FAIL() << "the replay did not diverge";
    } catch (const std::logic_error& error) {
        EXPECT_NE(std::string{error.what()}.find("recorded by crowdbook 0.1.0"),
                  std::string::npos);
    }
}

TEST(SessionTest, FilesSayWhichCrowdbookRecordedThem) {
    const Session session = playScripted().first;
    EXPECT_EQ(session.recordedBy, version());
    std::ostringstream file;
    writeSession(file, session);
    EXPECT_NE(file.str().find(std::format("recorded_by = \"crowdbook {}\"", version())),
              std::string::npos);
    EXPECT_EQ(parseSession(file.str()).recordedBy, version());
    // Older files do not say.
    const Session old = parseSession(
        "session_version = 1\nseed = 1\nend_ns = 100\n"
        "scenario = \"[[agents]]\\ntype = \\\"momentum\\\"\"\n");
    EXPECT_TRUE(old.recordedBy.empty());
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
        {"session_version = 3\nseed = 1\nend_ns = 100\n" + scenario, "test.session:1:"},
        {"session_version = 2\nseed = 1\nend_ns = 100\n" + scenario, "missing 'seats'"},
        {header + "seats = [\"you\"]\n", "a version 1 session has no 'seats'"},
        {"session_version = 2\nseed = 1\nend_ns = 100\nseats = []\n" + scenario,
         "one or more seat names"},
        {"session_version = 2\nseed = 1\nend_ns = 100\nseats = [\"a\", \"a\"]\n" + scenario,
         "seat 'a' is named twice"},
        {"session_version = 2\nseed = 1\nend_ns = 100\nseats = [\"a b\"]\n" + scenario,
         "a seat name has"},
        {"session_version = 2\nseed = 1\nend_ns = 100\nseats = [\"a\"]\n" + scenario +
             "actions = [{ time_ns = 5, instrument = 0, request = \"cancel\", "
             "client_order_id = 1 }]\n",
         "missing 'seat'"},
        {"session_version = 2\nseed = 1\nend_ns = 100\nseats = [\"a\"]\n" + scenario +
             "actions = [{ time_ns = 5, seat = 1, instrument = 0, request = \"cancel\", "
             "client_order_id = 1 }]\n",
         "'seat' must be a whole number between 0 and 0"},
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

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/simulation.hpp"
#include "test_agents.hpp"

namespace crowdbook {
namespace {

using test::add;
using test::Received;
using test::RecordingAgent;
using test::RecordingSink;

TEST(SimulationTest, RequestsAndRepliesTakeEachAgentsLatency) {
    Simulation simulation{1};
    RecordingSink sink;
    simulation.setEventSink(&sink);
    auto& alice =
        add<RecordingAgent>(simulation, {.latency = {.toExchange = 100, .fromExchange = 50}});
    alice.startHook = [](AgentContext& context) { context.submitLimit(Side::Buy, 99, 5); };
    auto& bob =
        add<RecordingAgent>(simulation, {.latency = {.toExchange = 10, .fromExchange = 20}});

    simulation.runUntil(1'000);

    ASSERT_EQ(sink.requests.size(), 1U);
    EXPECT_EQ(sink.requests[0].time, 100); // Alice's delay to the exchange
    const TopOfBook top{.bid = LevelSummary{.price = 99, .quantity = 5, .orderCount = 1}};
    EXPECT_EQ(alice.received, (std::vector<Received>{
                                  {.time = 150,
                                   .event = OrderAccepted{.agent = 1,
                                                          .clientOrderId = 1,
                                                          .orderId = 1,
                                                          .side = Side::Buy,
                                                          .price = 99,
                                                          .quantity = 5}},
                                  {.time = 150, .event = top},
                              }));
    // Bob only gets the public update, after his own delay from the exchange.
    EXPECT_EQ(bob.received, (std::vector<Received>{{.time = 120, .event = top}}));
}

TEST(SimulationTest, LinksKeepMessagesInOrderDespiteJitter) {
    Simulation simulation{1};
    RecordingSink sink;
    simulation.setEventSink(&sink);
    auto& alice = add<RecordingAgent>(
        simulation, {.latency = {.toExchange = 1'000, .fromExchange = 1'000, .jitter = 5'000}});
    alice.startHook = [](AgentContext& context) {
        for (int i = 0; i < 50; ++i) {
            context.submitLimit(Side::Buy, 90 + i % 5, 1);
        }
    };

    simulation.runUntil(kSecond);

    ASSERT_EQ(sink.requests.size(), 50U);
    for (std::size_t i = 0; i < sink.requests.size(); ++i) {
        EXPECT_EQ(clientOrderIdOf(sink.requests[i].request), i + 1);
        if (i > 0) {
            EXPECT_GE(sink.requests[i].time, sink.requests[i - 1].time);
        }
    }
    EXPECT_LT(sink.requests.front().time, sink.requests.back().time); // jitter did vary

    ClientOrderId expected = 1;
    Timestamp previous = 0;
    for (const Received& received : alice.received) {
        EXPECT_GE(received.time, previous);
        previous = received.time;
        if (const auto* accepted = std::get_if<OrderAccepted>(&received.event)) {
            EXPECT_EQ(accepted->clientOrderId, expected++);
        }
    }
    EXPECT_EQ(expected, 51U);
}

TEST(SimulationTest, WakeupsRunInTimeOrderWithTheirTags) {
    Simulation simulation{1};
    auto& agent = add<RecordingAgent>(simulation, {});
    agent.startHook = [](AgentContext& context) {
        context.wakeAt(300, 3);
        context.wakeAt(100, 1);
        context.wakeAfter(200, 2);
        context.wakeAt(-50, 0); // already past, so it runs as soon as possible
    };

    simulation.runUntil(1'000);

    EXPECT_EQ(agent.wakeups, (std::vector<std::pair<Timestamp, std::uint64_t>>{
                                 {0, 0}, {100, 1}, {200, 2}, {300, 3}}));
}

TEST(SimulationTest, AgentsStartAtTheirStartTimes) {
    Simulation simulation{1};
    auto& early = add<RecordingAgent>(simulation, {});
    auto& late = add<RecordingAgent>(simulation, {.startTime = 500});

    simulation.runUntil(1'000);

    EXPECT_EQ(early.starts, std::vector<Timestamp>{0});
    EXPECT_EQ(late.starts, std::vector<Timestamp>{500});
}

TEST(SimulationTest, RunUntilStopsAtTheEndTimeAndCanResume) {
    Simulation simulation{1};
    auto& agent = add<RecordingAgent>(simulation, {});
    agent.startHook = [](AgentContext& context) { context.wakeAt(1'000, 7); };

    simulation.runUntil(500);
    EXPECT_TRUE(agent.wakeups.empty());
    EXPECT_EQ(simulation.now(), 500);
    EXPECT_EQ(simulation.pendingCount(), 1U);

    simulation.runUntil(2'000);
    EXPECT_EQ(agent.wakeups, (std::vector<std::pair<Timestamp, std::uint64_t>>{{1'000, 7}}));
    EXPECT_EQ(simulation.now(), 2'000);
    EXPECT_EQ(simulation.pendingCount(), 0U);
}

TEST(SimulationTest, SimultaneousArrivalsKeepTheOrderTheyWereSent) {
    Simulation simulation{1};
    RecordingSink sink;
    simulation.setEventSink(&sink);
    auto& seller = add<RecordingAgent>(simulation, {.latency = {.toExchange = 100}});
    auto& buyer = add<RecordingAgent>(simulation, {.latency = {.toExchange = 100}});
    seller.startHook = [](AgentContext& context) { context.submitLimit(Side::Sell, 101, 1); };
    buyer.startHook = [](AgentContext& context) { context.submitLimit(Side::Buy, 101, 1); };

    simulation.runUntil(1'000);

    ASSERT_EQ(sink.requests.size(), 2U);
    EXPECT_EQ(sink.requests[0].time, sink.requests[1].time);
    EXPECT_EQ(sink.requests[0].agent, 1U);
    EXPECT_EQ(sink.requests[1].agent, 2U);
    // So the buy found the sell resting and traded with it.
    EXPECT_EQ(simulation.exchange().account(2).position, 1);
}

TEST(SimulationTest, LedgerShowsOrdersInFlightThenCatchesUp) {
    Simulation simulation{1};
    auto& alice =
        add<RecordingAgent>(simulation, {.latency = {.toExchange = 100, .fromExchange = 100}});
    bool wasInFlight = false;
    alice.startHook = [&wasInFlight](AgentContext& context) {
        const ClientOrderId id = context.submitLimit(Side::Sell, 100, 5);
        const OwnOrder* order = context.ledger().find(id);
        wasInFlight = order != nullptr && !order->acknowledged;
    };
    auto& bob = add<RecordingAgent>(simulation, {.startTime = 1'000});
    bob.startHook = [](AgentContext& context) { context.submitMarket(Side::Buy, 3); };

    simulation.runUntil(10'000);

    EXPECT_TRUE(wasInFlight);
    const Ledger& view = simulation.ledger(1);
    EXPECT_EQ(view.position(), -3);
    EXPECT_EQ(view.cash(), 300);
    ASSERT_NE(view.find(1), nullptr);
    EXPECT_TRUE(view.find(1)->acknowledged);
    EXPECT_EQ(view.find(1)->leaves, 2);
    for (const AgentId id : {AgentId{1}, AgentId{2}}) {
        EXPECT_EQ(simulation.ledger(id).cash(), simulation.exchange().account(id).cash);
        EXPECT_EQ(simulation.ledger(id).position(), simulation.exchange().account(id).position);
    }
}

// The first number each of two agents draws in a run with the given seed.
std::vector<std::uint64_t> firstDraws(std::uint64_t seed) {
    Simulation simulation{seed};
    std::vector<std::uint64_t> draws;
    for (int i = 0; i < 2; ++i) {
        auto& agent = add<RecordingAgent>(simulation, {});
        agent.startHook = [&draws](AgentContext& context) {
            draws.push_back(context.random().next());
        };
    }
    simulation.runUntil(0);
    return draws;
}

TEST(SimulationTest, RandomStreamsDependOnTheSeedAndTheAgent) {
    const std::vector<std::uint64_t> draws = firstDraws(7);
    ASSERT_EQ(draws.size(), 2U);
    EXPECT_NE(draws[0], draws[1]);
    EXPECT_EQ(firstDraws(7), draws);
    EXPECT_NE(firstDraws(8), draws);
}

TEST(SimulationTest, AddAgentRejectsInvalidOptions) {
    Simulation simulation{1};
    EXPECT_THROW(simulation.addAgent(nullptr), std::invalid_argument);
    EXPECT_THROW(simulation.addAgent(std::make_unique<RecordingAgent>(),
                                     {.latency = {.toExchange = -1}}),
                 std::invalid_argument);
    EXPECT_THROW(simulation.addAgent(std::make_unique<RecordingAgent>(),
                                     {.account = {.maxOrderQuantity = 0}}),
                 std::invalid_argument);
    simulation.runUntil(100);
    EXPECT_THROW(simulation.addAgent(std::make_unique<RecordingAgent>(), {.startTime = 50}),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(simulation.ledger(1)), std::out_of_range);
    // Failed calls do not use up ids.
    EXPECT_EQ(simulation.addAgent(std::make_unique<RecordingAgent>()), 1U);
}

TEST(SimulationTest, AgentAddedMidRunStartsNowByDefault) {
    Simulation simulation{1};
    simulation.runUntil(100);
    auto& agent = add<RecordingAgent>(simulation, {});

    simulation.runUntil(200);
    EXPECT_EQ(agent.starts, std::vector<Timestamp>{100});
}

} // namespace
} // namespace crowdbook

#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/agents/participant.hpp"
#include "crowdbook/live.hpp"
#include "crowdbook/simulation.hpp"
#include "exchange_test_support.hpp"
#include "test_agents.hpp"

namespace crowdbook {
namespace {

using test::add;
using test::limitOrder;
using test::RecordingAgent;

TEST(PacerTest, FollowsTheWallClockAtItsSpeed) {
    Pacer pacer{1'000, 50, 2.0}; // at wall time 1,000 the simulation is at 50
    EXPECT_EQ(pacer.simulatedAt(1'000), 50);
    EXPECT_EQ(pacer.simulatedAt(1'100), 250);
    EXPECT_EQ(pacer.simulatedAt(900), 50); // never before where it started
}

TEST(PacerTest, PausesAndChangesSpeedWithoutJumping) {
    Pacer pacer{0, 0};
    pacer.setPaused(true, 100);
    EXPECT_TRUE(pacer.paused());
    EXPECT_EQ(pacer.simulatedAt(5'000), 100);

    pacer.setPaused(false, 5'000);
    EXPECT_EQ(pacer.simulatedAt(5'100), 200);

    pacer.setSpeed(10.0, 5'100);
    EXPECT_EQ(pacer.simulatedAt(5'100), 200);
    EXPECT_EQ(pacer.simulatedAt(5'150), 700);
    EXPECT_EQ(pacer.speed(), 10.0);
    EXPECT_THROW(pacer.setSpeed(0.0, 6'000), std::invalid_argument);
    EXPECT_THROW((Pacer{0, 0, -1.0}), std::invalid_argument);
}

TEST(PerformTest, SendsTheRequestNowAndChecksItAgainstTheRecord) {
    Simulation simulation{1};
    auto& person = add<RecordingAgent>(simulation, {.latency = {.toExchange = 10}});
    simulation.runUntil(100);

    // Live, the id is not known yet; perform returns the one the order was given.
    EXPECT_EQ(perform(simulation, 1, {.time = 100, .request = limitOrder(0, Side::Buy, 99, 5)}),
              1U);
    simulation.runUntil(200);
    ASSERT_FALSE(person.received.empty());
    EXPECT_EQ(person.received.front().time, 110);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(person.received.front().event));

    EXPECT_EQ(perform(simulation, 1,
                      {.time = 200,
                       .request = ModifyOrder{.clientOrderId = 1, .price = 98, .quantity = 3}}),
              1U);
    EXPECT_EQ(perform(simulation, 1, {.time = 200, .request = CancelOrder{.clientOrderId = 1}}),
              1U);

    // A replay that would hand out a different id, or act at the wrong time, has diverged.
    EXPECT_THROW(perform(simulation, 1, {.time = 200, .request = limitOrder(5, Side::Buy, 99, 1)}),
                 std::logic_error);
    EXPECT_THROW(perform(simulation, 1, {.time = 150, .request = limitOrder(0, Side::Buy, 99, 1)}),
                 std::logic_error);
}

TEST(ParticipantTest, KeepsTheLatestTradesAndRejection) {
    Simulation simulation{1};
    auto& participant = add<Participant>(simulation, {}, 2);
    auto& seller = add<RecordingAgent>(simulation, {});
    seller.startHook = [](AgentContext& context) {
        for (const Price price : {101, 102, 103}) {
            context.submitLimit(Side::Sell, price, 1);
        }
    };
    simulation.runUntil(10);
    for (int i = 0; i < 3; ++i) {
        simulation.act(1, [](AgentContext& context) { context.submitMarket(Side::Buy, 1); });
        simulation.runUntil(20 + 10 * i);
    }
    simulation.act(1, [](AgentContext& context) { context.submitLimit(Side::Buy, 0, 1); });
    simulation.runUntil(100);

    ASSERT_EQ(participant.tape().size(), 2U);
    EXPECT_EQ(participant.tape()[0].trade.price, 103); // newest first
    EXPECT_EQ(participant.tape()[1].trade.price, 102);
    ASSERT_TRUE(participant.lastRejection().has_value());
    EXPECT_EQ(participant.lastRejection()->reason, RejectReason::InvalidPrice);
}

} // namespace
} // namespace crowdbook

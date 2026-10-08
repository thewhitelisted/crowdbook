#include <limits>
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

// The first wall-clock time at which simulated time reaches a moment, so that a server can sleep
// until then, exactly: any earlier and simulated time falls short.
TEST(PacerTest, SaysWhenSimulatedTimeWillReachAMoment) {
    for (const double speed : {1.0, 3.0, 0.3, 7.77}) {
        const Pacer pacer{1'000, 50, speed};
        EXPECT_EQ(pacer.wallAt(20), 1'000); // already past
        for (const Timestamp moment : {51, 52, 100, 12'345, 1'000'003}) {
            const std::optional<std::int64_t> wall = pacer.wallAt(moment);
            ASSERT_TRUE(wall);
            EXPECT_GE(pacer.simulatedAt(*wall), moment) << speed << " " << moment;
            EXPECT_LT(pacer.simulatedAt(*wall - 1), moment) << speed << " " << moment;
        }
    }
    Pacer paused{0, 0};
    paused.setPaused(true, 100);
    EXPECT_FALSE(paused.wallAt(200));
    EXPECT_EQ(paused.wallAt(50), 100);
    EXPECT_GT(Pacer(0, 0, 1e-9).wallAt(kMaxDuration), 0); // far off, but no overflow
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
    EXPECT_THROW(pacer.setSpeed(std::numeric_limits<double>::infinity(), 6'000),
                 std::invalid_argument);
    EXPECT_THROW((Pacer{0, 0, -1.0}), std::invalid_argument);
    EXPECT_THROW((Pacer{0, 0, std::numeric_limits<double>::quiet_NaN()}), std::invalid_argument);
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

TEST(ParticipantTest, KeepsTheLatestRejection) {
    Simulation simulation{1};
    auto& participant = add<Participant>(simulation, {});
    simulation.runUntil(10);
    simulation.act(1, [](AgentContext& context) { context.submitLimit(Side::Buy, 0, 1); });
    simulation.runUntil(100);

    ASSERT_TRUE(participant.lastRejection().has_value());
    EXPECT_EQ(participant.lastRejection()->reason, RejectReason::InvalidPrice);
}

} // namespace
} // namespace crowdbook

#include <stdexcept>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/agents/probe.hpp"

#include "fake_context.hpp"

namespace crowdbook {
namespace {

using test::FakeContext;

constexpr Price kReference = 1'000;

// Shows the probe a two-tick-wide market around `mid` and wakes it, as its timer would.
void tick(Probe& probe, FakeContext& context, Price mid) {
    context.snapshot = {.bid = LevelSummary{.price = mid - 1, .quantity = 1},
                        .ask = LevelSummary{.price = mid + 1, .quantity = 1}};
    probe.onWakeup(context, 0);
}

const NewOrder& order(const Request& request) { return std::get<NewOrder>(request); }

// A maker quotes its half width either side of the mid, rounded away from it, and requotes only
// when the price it wants moves.
TEST(ProbeTest, AMakerQuotesItsWidthAroundTheMid) {
    Probe probe{{.style = ProbeStyle::Maker, .size = 2, .offset = 2.5}, kReference};
    FakeContext context;
    tick(probe, context, 1'000);
    std::vector<Request> sent = context.takeSent();
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(order(sent[0]).side, Side::Buy);
    EXPECT_EQ(order(sent[0]).price, 997);
    EXPECT_EQ(order(sent[0]).quantity, 2);
    EXPECT_EQ(order(sent[1]).price, 1'003);

    tick(probe, context, 1'000);
    EXPECT_TRUE(context.takeSent().empty());

    // The mid moves up a tick: both quotes are cancelled and placed again a tick higher.
    tick(probe, context, 1'001);
    sent = context.takeSent();
    ASSERT_EQ(sent.size(), 4U);
    EXPECT_TRUE(std::holds_alternative<CancelOrder>(sent[0]));
    EXPECT_EQ(order(sent[1]).price, 998);
    EXPECT_EQ(order(sent[3]).price, 1'004);
}

// A trend follower buys at the ask when the mid rises, up to its limit; a reversal trader sells.
TEST(ProbeTest, TrendAndReversalTakeOppositeSides) {
    for (const auto style : {ProbeStyle::Trend, ProbeStyle::Reversal}) {
        Probe probe{{.style = style,
                     .interval = 100 * kMillisecond,
                     .size = 5,
                     .maxPosition = 10,
                     .fastHalfLife = 100 * kMillisecond,
                     .slowHalfLife = 10 * kSecond},
                    kReference};
        FakeContext context;
        for (Price mid = 1'000; mid <= 1'050; mid += 5) {
            tick(probe, context, mid);
        }
        const std::vector<Request> sent = context.takeSent();
        ASSERT_EQ(sent.size(), 2U); // two orders of 5 reach the limit of 10
        const Side side = style == ProbeStyle::Trend ? Side::Buy : Side::Sell;
        EXPECT_EQ(order(sent[0]).side, side);
        EXPECT_EQ(order(sent[0]).timeInForce, TimeInForce::ImmediateOrCancel);
        EXPECT_EQ(order(sent[0]).price, side == Side::Buy ? 1'006 : 1'004);
    }
}

// A flow follower buys when most of the recent volume was bought, and forgets trades older than
// its window.
TEST(ProbeTest, AFlowFollowerTakesWithTheTakers) {
    Probe probe{{.style = ProbeStyle::Flow, .size = 1, .window = 10 * kSecond}, kReference};
    FakeContext context;
    probe.onTrade(context, Trade{.price = 1'000, .quantity = 8, .aggressorSide = Side::Buy});
    probe.onTrade(context, Trade{.price = 1'000, .quantity = 2, .aggressorSide = Side::Sell});
    tick(probe, context, 1'000);
    std::vector<Request> sent = context.takeSent();
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(order(sent[0]).side, Side::Buy);

    context.setNow(20 * kSecond);
    probe.onTrade(context, Trade{.price = 1'000, .quantity = 9, .aggressorSide = Side::Sell});
    tick(probe, context, 1'000);
    sent = context.takeSent();
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(order(sent[0]).side, Side::Sell);
}

TEST(ProbeTest, SettingsAreChecked) {
    for (const ProbeConfig& config :
         {ProbeConfig{.interval = 0}, ProbeConfig{.size = 0}, ProbeConfig{.size = 20},
          ProbeConfig{.offset = -1.0}, ProbeConfig{.fastHalfLife = 60 * kSecond},
          ProbeConfig{.imbalance = 0.4}, ProbeConfig{.imbalance = 1.0}}) {
        EXPECT_THROW((Probe{config, kReference}), std::invalid_argument);
    }
}

} // namespace
} // namespace crowdbook

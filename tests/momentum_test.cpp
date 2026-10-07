#include <cmath>
#include <stdexcept>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/agents/momentum.hpp"
#include "exchange_test_support.hpp"
#include "fake_context.hpp"

namespace crowdbook {
namespace {

using test::FakeContext;
using test::marketOrder;

constexpr Price kReference = 1'000;
const MomentumConfig kConfig{.interval = 100 * kMillisecond,
                             .fastHalfLife = 100 * kMillisecond,
                             .slowHalfLife = 10 * kSecond,
                             .threshold = 2.0,
                             .orderSize = 5,
                             .maxPosition = 15};

// Shows the trader a two-tick-wide market around `mid` and wakes it, as its timer would.
void tick(MomentumTrader& trader, FakeContext& context, Price mid) {
    context.snapshot = {.bid = LevelSummary{.price = mid - 1, .quantity = 1},
                        .ask = LevelSummary{.price = mid + 1, .quantity = 1}};
    trader.onWakeup(context, 0);
}

TEST(MomentumTest, AveragesMoveWithTheirHalfLives) {
    MomentumTrader trader{kConfig, kReference};
    FakeContext context;
    tick(trader, context, 1'000);
    EXPECT_EQ(trader.trend(), 0.0);

    // The fast average covers half the jump in one interval; the slow one barely moves.
    tick(trader, context, 1'010);
    const double slowWeight = 1.0 - std::exp2(-0.01);
    EXPECT_NEAR(trader.trend(), 5.0 - 10.0 * slowWeight, 1e-9);
}

TEST(MomentumTest, BuysIntoARisingMarketUpToItsLimit) {
    MomentumTrader trader{kConfig, kReference};
    FakeContext context;
    for (Price mid = 1'000; mid <= 1'100; mid += 5) {
        tick(trader, context, mid);
    }
    // Nothing fills here, so orders in flight count against the limit: three buys of 5 make 15.
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{marketOrder(1, Side::Buy, 5),
                                                        marketOrder(2, Side::Buy, 5),
                                                        marketOrder(3, Side::Buy, 5)}));
}

TEST(MomentumTest, SellsIntoAFallingMarket) {
    MomentumTrader trader{kConfig, kReference};
    FakeContext context;
    for (Price mid = 1'000; mid >= 900; mid -= 5) {
        tick(trader, context, mid);
    }
    const std::vector<Request> sent = context.takeSent();
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(std::get<NewOrder>(sent[0]).side, Side::Sell);
}

TEST(MomentumTest, StaysOutOfAFlatMarket) {
    MomentumTrader trader{kConfig, kReference};
    FakeContext context;
    for (int i = 0; i < 100; ++i) {
        tick(trader, context, 1'000);
    }
    EXPECT_TRUE(context.takeSent().empty());
    EXPECT_EQ(context.wakeups.size(), 100U); // it keeps checking
}

TEST(MomentumTest, RejectsInvalidConfigs) {
    EXPECT_THROW((MomentumTrader{{.fastHalfLife = kSecond, .slowHalfLife = kSecond}, kReference}),
                 std::invalid_argument);
    EXPECT_THROW((MomentumTrader{{.interval = 0}, kReference}), std::invalid_argument);
    EXPECT_THROW((MomentumTrader{{.threshold = -1.0}, kReference}), std::invalid_argument);
    EXPECT_THROW((MomentumTrader{{.orderSize = 10, .maxPosition = 5}, kReference}),
                 std::invalid_argument);
}

} // namespace
} // namespace crowdbook

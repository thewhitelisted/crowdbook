#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/agents/informed.hpp"
#include "exchange_test_support.hpp"
#include "fake_context.hpp"

namespace crowdbook {
namespace {

using test::FakeContext;
using test::limitOrder;

// A fundamental that never moves, so the tests know the value exactly.
std::shared_ptr<Fundamental> fixedValue(double value) {
    return std::make_shared<Fundamental>(FundamentalConfig{.initial = value, .volatility = 0.0},
                                         Random{1, 0});
}

const InformedConfig kExact{.interval = 100 * kMillisecond,
                            .belief = {.noise = 0.0},
                            .threshold = 3.0,
                            .orderSize = 5,
                            .maxPosition = 10};

const MarketSnapshot kMarket{.bid = LevelSummary{.price = 98, .quantity = 1},
                             .ask = LevelSummary{.price = 100, .quantity = 1}};

std::vector<Request> lookOnce(InformedTrader& trader, FakeContext& context) {
    context.snapshot = kMarket;
    trader.onWakeup(context, 0);
    return context.takeSent();
}

TEST(InformedTest, BuysWhenTheAskIsWellBelowItsEstimate) {
    InformedTrader trader{kExact, fixedValue(105.0)};
    FakeContext context;
    // Edge 105 - 100 = 5 >= 3, and the limit keeps 3 ticks of it: never pays more than 102.
    EXPECT_EQ(lookOnce(trader, context),
              (std::vector<Request>{
                  limitOrder(1, Side::Buy, 102, 5, TimeInForce::ImmediateOrCancel)}));
}

TEST(InformedTest, SellsWhenTheBidIsWellAboveItsEstimate) {
    InformedTrader trader{kExact, fixedValue(94.5)};
    FakeContext context;
    EXPECT_EQ(lookOnce(trader, context),
              (std::vector<Request>{
                  limitOrder(1, Side::Sell, 98, 5, TimeInForce::ImmediateOrCancel)}));
}

TEST(InformedTest, WaitsWhenTheEdgeIsTooSmall) {
    InformedTrader trader{kExact, fixedValue(101.0)};
    FakeContext context;
    EXPECT_TRUE(lookOnce(trader, context).empty());
    EXPECT_EQ(context.wakeups.size(), 1U); // and looks again later
}

TEST(InformedTest, StopsAtItsPositionLimit) {
    InformedTrader trader{kExact, fixedValue(105.0)};
    FakeContext context;
    std::size_t orders = 0;
    for (int i = 0; i < 10; ++i) {
        orders += lookOnce(trader, context).size();
    }
    EXPECT_EQ(orders, 2U); // two orders of 5 in flight reach the limit of 10
}

TEST(InformedTest, FirstLooksAtARandomPointInItsFirstInterval) {
    const std::vector<Timestamp> first = test::firstWakeups(
        [] { return std::make_unique<InformedTrader>(kExact, fixedValue(100.0)); }, 20);
    for (const Timestamp time : first) {
        EXPECT_GT(time, 0);
        EXPECT_LE(time, kExact.interval);
    }
    // Traders started together spread out instead of looking in lockstep.
    EXPECT_GT(std::set<Timestamp>(first.begin(), first.end()).size(), 15U);
}

TEST(InformedTest, RequiresAFundamentalAndAValidConfig) {
    EXPECT_THROW((InformedTrader{kExact, nullptr}), std::invalid_argument);
    EXPECT_THROW((InformedTrader{{.interval = 0}, fixedValue(100.0)}), std::invalid_argument);
    EXPECT_THROW((InformedTrader{{.belief = {.noise = -1.0}}, fixedValue(100.0)}), std::invalid_argument);
    EXPECT_THROW((InformedTrader{{.orderSize = 6, .maxPosition = 5}, fixedValue(100.0)}),
                 std::invalid_argument);
    EXPECT_THROW((InformedTrader{{.maxPosition = kMaxQuantity + 1}, fixedValue(100.0)}),
                 std::invalid_argument);
}

TEST(InformedTest, PricesAWildEstimateAtTheExchangesLimits) {
    // A value beyond any price the exchange takes buys at the highest price it does take.
    InformedTrader high{kExact, fixedValue(1e15)};
    FakeContext context;
    EXPECT_EQ(lookOnce(high, context),
              (std::vector<Request>{
                  limitOrder(1, Side::Buy, kMaxPrice, 5, TimeInForce::ImmediateOrCancel)}));
    // And a value below zero sells at the lowest.
    InformedTrader low{kExact, fixedValue(-50.0)};
    FakeContext other;
    EXPECT_EQ(lookOnce(low, other),
              (std::vector<Request>{
                  limitOrder(1, Side::Sell, 1, 5, TimeInForce::ImmediateOrCancel)}));
}

} // namespace
} // namespace crowdbook

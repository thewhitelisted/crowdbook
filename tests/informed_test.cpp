#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <utility>
#include <stdexcept>
#include <variant>
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

// Spread over the looks, the mean and coefficient of variation of the waits it asked for.
std::pair<double, double> waits(const InformedConfig& config, int looks) {
    InformedTrader trader{config, fixedValue(101.0)};
    FakeContext context{3};
    for (int i = 0; i < looks; ++i) {
        lookOnce(trader, context);
    }
    double sum = 0.0;
    double squares = 0.0;
    for (const auto& [time, tag] : context.wakeups) {
        const double seconds = static_cast<double>(time) / static_cast<double>(kSecond);
        sum += seconds;
        squares += seconds * seconds;
    }
    const double n = static_cast<double>(context.wakeups.size());
    const double mean = sum / n;
    return {mean, std::sqrt(squares / n - mean * mean) / mean};
}

TEST(InformedTest, LooksOnItsIntervalOrAtRandomWithItsMean) {
    InformedConfig every = kExact;
    every.interval = kSecond;
    const auto [fixedMean, fixedSpread] = waits(every, 4000);
    EXPECT_DOUBLE_EQ(fixedMean, 1.0);
    EXPECT_NEAR(fixedSpread, 0.0, 1e-6);
    // Waits drawn from an exponential: the same mean, and as variable as their mean.
    InformedConfig poisson = every;
    poisson.randomInterval = true;
    const auto [mean, spread] = waits(poisson, 4000);
    EXPECT_NEAR(mean, 1.0, 0.05);
    EXPECT_NEAR(spread, 1.0, 0.05);
    // Its first look too, where a fixed interval starts uniformly within the first.
    const auto first = test::firstWakeups(
        [&] { return std::make_unique<InformedTrader>(poisson, fixedValue(101.0)); }, 4000);
    const auto beyond = std::count_if(first.begin(), first.end(),
                                      [](Timestamp t) { return t > kSecond; });
    EXPECT_NEAR(static_cast<double>(beyond) / 4000.0, std::exp(-1.0), 0.03);
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

TEST(InformedTest, DrawsSizesFromAParetoTail) {
    // P(size >= q) = (5 / q)^1.5: about 3% of orders are 50 lots or more.
    InformedConfig config = kExact;
    config.maxPosition = 100'000;
    config.sizeTail = 1.5;
    int orders = 0;
    int large = 0;
    for (std::uint64_t seed = 1; seed <= 4'000; ++seed) {
        InformedTrader trader{config, fixedValue(105.0)};
        FakeContext context(seed);
        for (const Request& request : lookOnce(trader, context)) {
            const Quantity size = std::get<NewOrder>(request).quantity;
            EXPECT_GE(size, config.orderSize);
            large += size >= 50 ? 1 : 0;
            ++orders;
        }
    }
    EXPECT_EQ(orders, 4'000);
    EXPECT_NEAR(large / 4'000.0, 0.0316, 0.01);
}

TEST(InformedTest, CutsADrawnSizeToTheRoomItsLimitLeaves) {
    InformedConfig config = kExact;
    config.maxPosition = 12;
    config.sizeTail = 0.2; // so heavy that nearly every draw is past the limit
    InformedTrader trader{config, fixedValue(105.0)};
    FakeContext context;
    Quantity inFlight = 0;
    for (int i = 0; i < 10; ++i) {
        for (const Request& request : lookOnce(trader, context)) {
            inFlight += std::get<NewOrder>(request).quantity;
        }
    }
    EXPECT_LE(inFlight, config.maxPosition);
    EXPECT_GE(inFlight, config.maxPosition - config.orderSize + 1);
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

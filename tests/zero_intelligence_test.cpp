#include <cstdint>
#include <set>
#include <stdexcept>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/agents/zero_intelligence.hpp"
#include "fake_context.hpp"

namespace crowdbook {
namespace {

using test::FakeContext;

constexpr Price kReference = 1'000;
constexpr int kOrders = 7'000;

// Fires the trader's order timer `orders` times and returns everything it sent. The fake clock
// stays at 0, so each recorded wakeup time is the delay that was asked for.
std::vector<Request> run(ZeroIntelligenceTrader& trader, FakeContext& context, int orders) {
    trader.onStart(context);
    for (int i = 0; i < orders; ++i) {
        trader.onWakeup(context, 0);
    }
    return context.takeSent();
}

double meanSeconds(const FakeContext& context, bool orderTimer) {
    double total = 0.0;
    int count = 0;
    for (const auto& [time, tag] : context.wakeups) {
        if ((tag == 0) == orderTimer) {
            total += static_cast<double>(time) / 1e9;
            ++count;
        }
    }
    return total / count;
}

TEST(ZeroIntelligenceTest, WaitsExponentiallyBetweenOrders) {
    ZeroIntelligenceTrader trader{{.limitRate = 2.0, .marketRate = 0.5}, kReference};
    FakeContext context;
    static_cast<void>(run(trader, context, kOrders));
    EXPECT_NEAR(meanSeconds(context, true), 1.0 / 2.5, 0.4 * 0.05);
}

TEST(ZeroIntelligenceTest, MixesLimitAndMarketOrdersByTheirRates) {
    ZeroIntelligenceTrader trader{{.limitRate = 2.0, .marketRate = 0.5}, kReference};
    FakeContext context;
    int limits = 0;
    int markets = 0;
    for (const Request& request : run(trader, context, kOrders)) {
        (std::get<NewOrder>(request).type == OrderType::Limit ? limits : markets) += 1;
    }
    EXPECT_NEAR(limits, kOrders * 0.8, kOrders * 0.02);
    EXPECT_EQ(limits + markets, kOrders);
}

TEST(ZeroIntelligenceTest, GivesEachLimitOrderAnExponentialLifetime) {
    ZeroIntelligenceTrader trader{{.limitRate = 1.0, .marketRate = 0.0, .cancelRate = 0.5},
                                  kReference};
    FakeContext context;
    std::set<std::uint64_t> placed;
    for (const Request& request : run(trader, context, kOrders)) {
        placed.insert(clientOrderIdOf(request));
    }
    std::set<std::uint64_t> timed;
    for (const auto& [time, tag] : context.wakeups) {
        if (tag != 0) {
            timed.insert(tag);
        }
    }
    EXPECT_EQ(timed, placed); // one lifetime per order, tagged with its client order id
    EXPECT_NEAR(meanSeconds(context, false), 1.0 / 0.5, 2.0 * 0.05);
}

TEST(ZeroIntelligenceTest, CancelsAnOrderWhenItsLifetimeEndsUnlessItFilled) {
    ZeroIntelligenceTrader trader{{.limitRate = 1.0, .marketRate = 0.0}, kReference};
    FakeContext context;
    static_cast<void>(run(trader, context, 2)); // orders 1 and 2
    context.accept(1);
    context.accept(2);
    context.fill(2, context.ledger().find(2)->leaves); // order 2 fills completely

    trader.onWakeup(context, 1);
    trader.onWakeup(context, 1); // already being cancelled: nothing more to send
    trader.onWakeup(context, 2); // already filled: nothing to cancel
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{CancelOrder{.clientOrderId = 1}}));
}

TEST(ZeroIntelligenceTest, KeepsOrdersRestingWithoutACancelRate) {
    ZeroIntelligenceTrader trader{{.limitRate = 1.0, .marketRate = 0.0, .cancelRate = 0.0},
                                  kReference};
    FakeContext context;
    static_cast<void>(run(trader, context, 100));
    for (const auto& [time, tag] : context.wakeups) {
        EXPECT_EQ(tag, 0U);
    }
}

TEST(ZeroIntelligenceTest, LimitOrdersSitInsideTheOppositeQuoteWithoutCrossing) {
    ZeroIntelligenceTrader trader{{.marketRate = 0.0, .maxOffset = 5}, kReference};
    FakeContext context;
    context.snapshot = {.bid = LevelSummary{.price = 990, .quantity = 1},
                        .ask = LevelSummary{.price = 1'010, .quantity = 1}};

    std::set<Price> buyPrices;
    std::set<Price> sellPrices;
    for (const Request& request : run(trader, context, 2'000)) {
        const auto& order = std::get<NewOrder>(request);
        (order.side == Side::Buy ? buyPrices : sellPrices).insert(order.price);
    }
    EXPECT_EQ(buyPrices, (std::set<Price>{1'005, 1'006, 1'007, 1'008, 1'009}));
    EXPECT_EQ(sellPrices, (std::set<Price>{991, 992, 993, 994, 995}));
}

TEST(ZeroIntelligenceTest, AnchorsOnTheReferencePriceBeforeSeeingAnyQuotes) {
    ZeroIntelligenceTrader trader{{.marketRate = 0.0, .maxOffset = 2}, kReference};
    FakeContext context;
    std::set<Price> buyPrices;
    std::set<Price> sellPrices;
    for (const Request& request : run(trader, context, 500)) {
        const auto& order = std::get<NewOrder>(request);
        (order.side == Side::Buy ? buyPrices : sellPrices).insert(order.price);
    }
    EXPECT_EQ(buyPrices, (std::set<Price>{999, 1'000}));
    EXPECT_EQ(sellPrices, (std::set<Price>{1'000, 1'001}));
}

// Shows the trader the market's trade count so far and fires its order timer at `time`.
void tick(ZeroIntelligenceTrader& trader, FakeContext& context, Timestamp time,
          std::uint64_t trades) {
    context.setNow(time);
    context.snapshot.trades = trades;
    trader.onWakeup(context, 0);
}

TEST(ZeroIntelligenceTest, PaceFollowsRecentActivityAgainstItsUsualLevel) {
    const ZeroIntelligenceConfig steady{.limitRate = 1.0, .marketRate = 0.0};
    ZeroIntelligenceConfig responsive = steady;
    responsive.activityResponse = 1.0;
    responsive.activityMemory = kSecond;
    responsive.activityBaseline = 100 * kSecond;
    ZeroIntelligenceTrader trader{responsive, kReference};
    ZeroIntelligenceTrader constant{steady, kReference}; // the same draws, at a steady pace
    FakeContext context;
    FakeContext constantContext;
    trader.onStart(context);
    constant.onStart(constantContext);

    Timestamp time = 0;
    std::uint64_t trades = 0;
    const auto seconds = [&](int count, std::uint64_t perSecond) {
        for (int i = 0; i < count; ++i) {
            time += kSecond;
            trades += perSecond;
            tick(trader, context, time, trades);
            tick(constant, constantContext, time, trades);
        }
    };

    seconds(60, 10); // a steady 10 trades a second: recent and usual agree
    EXPECT_NEAR(trader.pace(), 1.0, 0.01);
    seconds(5, 100); // a burst of 100 a second: it speeds up about fivefold
    EXPECT_GT(trader.pace(), 4.0);
    // Its next order comes sooner by exactly that factor.
    const auto lastDelay = [&time](const FakeContext& fake) {
        return static_cast<double>(fake.wakeups.back().first - time);
    };
    EXPECT_NEAR(lastDelay(constantContext) / lastDelay(context), trader.pace(), 1e-3);
    seconds(10, 10); // quiet again, while the burst still weighs on its usual level: it slows
    EXPECT_LT(trader.pace(), 1.0);
    EXPECT_EQ(constant.pace(), 1.0);
}

TEST(ZeroIntelligenceTest, RejectsInvalidConfigs) {
    EXPECT_THROW((ZeroIntelligenceTrader{{.limitRate = -1.0}, kReference}), std::invalid_argument);
    EXPECT_THROW((ZeroIntelligenceTrader{{.limitRate = 0.0, .marketRate = 0.0}, kReference}),
                 std::invalid_argument);
    EXPECT_THROW((ZeroIntelligenceTrader{{.cancelRate = -0.1}, kReference}),
                 std::invalid_argument);
    EXPECT_THROW((ZeroIntelligenceTrader{{.maxOffset = 0}, kReference}), std::invalid_argument);
    EXPECT_THROW((ZeroIntelligenceTrader{{.minSize = 5, .maxSize = 4}, kReference}),
                 std::invalid_argument);
    EXPECT_THROW((ZeroIntelligenceTrader{{.activityResponse = -0.5}, kReference}),
                 std::invalid_argument);
    EXPECT_THROW((ZeroIntelligenceTrader{{.activityMemory = 0}, kReference}),
                 std::invalid_argument);
    EXPECT_THROW((ZeroIntelligenceTrader{
                     {.activityMemory = 10 * kSecond, .activityBaseline = kSecond}, kReference}),
                 std::invalid_argument);
}

} // namespace
} // namespace crowdbook

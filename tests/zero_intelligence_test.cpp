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
constexpr int kWakeups = 7'000;

// Wakes the trader repeatedly and returns everything it sent.
std::vector<Request> run(ZeroIntelligenceTrader& trader, FakeContext& context, int wakeups) {
    trader.onStart(context);
    for (int i = 0; i < wakeups; ++i) {
        trader.onWakeup(context, 0);
    }
    return context.takeSent();
}

TEST(ZeroIntelligenceTest, WaitsExponentiallyWithTheTotalRate) {
    ZeroIntelligenceTrader trader{{.limitRate = 2.0, .marketRate = 0.5, .cancelRate = 1.5},
                                  kReference};
    FakeContext context;
    static_cast<void>(run(trader, context, kWakeups));

    double total = 0.0;
    for (const auto& wakeup : context.wakeups) {
        total += static_cast<double>(wakeup.first); // the clock stays at 0, so this is the delay
    }
    const double meanSeconds = total / static_cast<double>(context.wakeups.size()) / 1e9;
    EXPECT_NEAR(meanSeconds, 1.0 / 4.0, 0.25 * 0.05);
}

TEST(ZeroIntelligenceTest, MixesActionsInProportionToTheirRates) {
    ZeroIntelligenceTrader trader{{.limitRate = 2.0, .marketRate = 0.5, .cancelRate = 1.5},
                                  kReference};
    FakeContext context;
    int limits = 0;
    int markets = 0;
    int cancels = 0;
    for (const Request& request : run(trader, context, kWakeups)) {
        if (const auto* order = std::get_if<NewOrder>(&request)) {
            (order->type == OrderType::Limit ? limits : markets) += 1;
        } else if (std::holds_alternative<CancelOrder>(request)) {
            ++cancels;
        }
    }
    // Nothing ever fills here, so there is always an order to cancel.
    EXPECT_NEAR(limits, kWakeups * 2.0 / 4.0, kWakeups * 0.02);
    EXPECT_NEAR(markets, kWakeups * 0.5 / 4.0, kWakeups * 0.02);
    EXPECT_NEAR(cancels, kWakeups * 1.5 / 4.0, kWakeups * 0.02);
}

TEST(ZeroIntelligenceTest, LimitOrdersSitInsideTheOppositeQuoteWithoutCrossing) {
    ZeroIntelligenceTrader trader{{.marketRate = 0.0, .cancelRate = 0.0, .maxOffset = 5},
                                  kReference};
    FakeContext context;
    trader.onTopOfBook(context, {.bid = LevelSummary{.price = 990, .quantity = 1},
                                 .ask = LevelSummary{.price = 1'010, .quantity = 1}});

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
    ZeroIntelligenceTrader trader{{.marketRate = 0.0, .cancelRate = 0.0, .maxOffset = 2},
                                  kReference};
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

TEST(ZeroIntelligenceTest, CancelsEachOfItsOwnOrdersAtMostOnce) {
    ZeroIntelligenceTrader trader{{.limitRate = 1.0, .marketRate = 0.0, .cancelRate = 3.0},
                                  kReference};
    FakeContext context;
    std::set<ClientOrderId> placed;
    std::set<ClientOrderId> cancelled;
    for (const Request& request : run(trader, context, 2'000)) {
        if (const auto* cancel = std::get_if<CancelOrder>(&request)) {
            EXPECT_TRUE(placed.contains(cancel->clientOrderId));
            EXPECT_TRUE(cancelled.insert(cancel->clientOrderId).second);
        } else {
            placed.insert(clientOrderIdOf(request));
        }
    }
    EXPECT_FALSE(cancelled.empty());
}

TEST(ZeroIntelligenceTest, RejectsInvalidConfigs) {
    EXPECT_THROW((ZeroIntelligenceTrader{{.limitRate = -1.0}, kReference}), std::invalid_argument);
    EXPECT_THROW(
        (ZeroIntelligenceTrader{{.limitRate = 0.0, .marketRate = 0.0, .cancelRate = 0.0},
                                kReference}),
        std::invalid_argument);
    EXPECT_THROW((ZeroIntelligenceTrader{{.maxOffset = 0}, kReference}), std::invalid_argument);
    EXPECT_THROW((ZeroIntelligenceTrader{{.minSize = 5, .maxSize = 4}, kReference}),
                 std::invalid_argument);
}

} // namespace
} // namespace crowdbook

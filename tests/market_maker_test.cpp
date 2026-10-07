#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/agents/market_maker.hpp"
#include "exchange_test_support.hpp"
#include "fake_context.hpp"

namespace crowdbook {
namespace {

using test::FakeContext;
using test::limitOrder;

constexpr Price kReference = 100;

// gamma * sigma^2 * tau = 0.1 * 4 * 1 = 0.4, so each lot of inventory moves the reservation price
// 0.4 ticks, and the half spread is 0.2 + ln(1 + 0.1 / 1.5) / 0.1 = 0.845 ticks.
const MarketMakerConfig kConfig{.riskAversion = 0.1,
                                .volatility = 2.0,
                                .intensity = 1.5,
                                .horizon = kSecond,
                                .quoteSize = 5,
                                .maxInventory = 50,
                                .requoteInterval = 100 * kMillisecond};

const TopOfBook kTop{.bid = LevelSummary{.price = 99, .quantity = 1},
                     .ask = LevelSummary{.price = 101, .quantity = 1}};

TEST(AvellanedaStoikovTest, QuotesLeanAgainstInventory) {
    EXPECT_EQ(avellanedaStoikovQuotes(kConfig, 100.0, 0), (Quotes{.bid = 99, .ask = 101}));
    // Long 10 lots: r = 100 - 10 * 0.4 = 96, so the quotes move down to sell inventory.
    EXPECT_EQ(avellanedaStoikovQuotes(kConfig, 100.0, 10), (Quotes{.bid = 95, .ask = 97}));
    EXPECT_EQ(avellanedaStoikovQuotes(kConfig, 100.0, -10), (Quotes{.bid = 103, .ask = 105}));
}

TEST(AvellanedaStoikovTest, LeavesOutASideThatWouldBreachTheInventoryLimit) {
    EXPECT_EQ(avellanedaStoikovQuotes(kConfig, 100.0, 46).bid, std::nullopt); // 46 + 5 > 50
    EXPECT_TRUE(avellanedaStoikovQuotes(kConfig, 100.0, 46).ask.has_value());
    EXPECT_EQ(avellanedaStoikovQuotes(kConfig, 100.0, -46).ask, std::nullopt);
    EXPECT_TRUE(avellanedaStoikovQuotes(kConfig, 100.0, -46).bid.has_value());
}

TEST(AvellanedaStoikovTest, KeepsWildQuotesWithinTheExchangesPrices) {
    // Risk this high puts the half spread near 2e20 ticks, far past what a price can hold.
    MarketMakerConfig wild = kConfig;
    wild.riskAversion = 1e20;
    EXPECT_EQ(avellanedaStoikovQuotes(wild, 100.0, 0), (Quotes{.ask = kMaxPrice}));
    EXPECT_EQ(avellanedaStoikovQuotes(wild, 100.0, -10),
              (Quotes{.bid = kMaxPrice, .ask = kMaxPrice}));
    EXPECT_EQ(avellanedaStoikovQuotes(wild, 100.0, 10), (Quotes{.ask = 1}));
}

TEST(MarketMakerTest, QuotesBothSidesOnStartAndRequotesOnATimer) {
    MarketMaker maker{kConfig, kReference};
    FakeContext context;
    maker.onTopOfBook(context, kTop);

    maker.onStart(context);
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{limitOrder(1, Side::Buy, 99, 5),
                                                        limitOrder(2, Side::Sell, 101, 5)}));
    ASSERT_EQ(context.wakeups.size(), 1U);
    EXPECT_GT(context.wakeups[0].first, 0);
    EXPECT_LE(context.wakeups[0].first, 100 * kMillisecond);

    // Nothing changed, so the timer sends nothing.
    context.setNow(100 * kMillisecond);
    maker.onWakeup(context, 0);
    EXPECT_TRUE(context.takeSent().empty());
}

TEST(MarketMakerTest, RequotesStraightAfterAFill) {
    MarketMaker maker{kConfig, kReference};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context);
    static_cast<void>(context.takeSent());
    context.accept(1);
    context.accept(2);

    // The whole bid fills: long 5 lots, r = 98, so a new bid at 97 and the ask moves to 99.
    context.fill(1, 5);
    maker.onFilled(context, {});
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{limitOrder(3, Side::Buy, 97, 5),
                                                        ModifyOrder{.clientOrderId = 2,
                                                                    .price = 99,
                                                                    .quantity = 5}}));
}

TEST(MarketMakerTest, TopsUpAPartlyFilledQuote) {
    MarketMaker maker{kConfig, kReference};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context);
    static_cast<void>(context.takeSent());
    context.accept(1);
    context.accept(2);

    // Two lots of the ask fill: short 2, r = 100.8, so bid 99 and ask 102, back to 5 lots.
    context.fill(2, 2);
    maker.onFilled(context, {});
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{ModifyOrder{.clientOrderId = 2,
                                                                    .price = 102,
                                                                    .quantity = 5}}));
}

TEST(MarketMakerTest, CancelsTheQuoteThatWouldBreachItsLimit) {
    MarketMakerConfig config = kConfig;
    config.maxInventory = 5;
    MarketMaker maker{config, kReference};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context);
    static_cast<void>(context.takeSent());
    context.accept(1);
    context.accept(2);

    // Long 5 = the limit: no new bid, and the ask moves down to sell (r = 98).
    context.fill(1, 5);
    maker.onFilled(context, {});
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{ModifyOrder{.clientOrderId = 2,
                                                                    .price = 99,
                                                                    .quantity = 5}}));
}

TEST(MarketMakerTest, PostOnlyQuotesStayOffTheFarSideOfTheBook) {
    MarketMakerConfig config = kConfig;
    config.postOnly = true;
    MarketMaker maker{config, kReference};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context);
    EXPECT_EQ(context.takeSent(),
              (std::vector<Request>{limitOrder(1, Side::Buy, 99, 5, TimeInForce::PostOnly),
                                    limitOrder(2, Side::Sell, 101, 5, TimeInForce::PostOnly)}));
    context.accept(1);
    context.accept(2);

    // Long 5 after the bid fills, r = 98 wants the ask at 99, but that would trade with the bid at
    // 99 it last saw, so the ask only comes down to 100.
    context.fill(1, 5);
    maker.onFilled(context, {});
    EXPECT_EQ(context.takeSent(),
              (std::vector<Request>{limitOrder(3, Side::Buy, 97, 5, TimeInForce::PostOnly),
                                    ModifyOrder{.clientOrderId = 2, .price = 100, .quantity = 5}}));
}

TEST(MarketMakerTest, SendsARejectedModifyAgain) {
    MarketMaker maker{kConfig, kReference};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context);
    static_cast<void>(context.takeSent());
    context.accept(1);
    context.accept(2);

    // A trade at 103 moves the fair price there, so the quotes move to 102 and 104.
    maker.onTrade(context, {.price = 103, .quantity = 1});
    maker.onWakeup(context, 0);
    ASSERT_EQ(context.takeSent(),
              (std::vector<Request>{ModifyOrder{.clientOrderId = 1, .price = 102, .quantity = 5},
                                    ModifyOrder{.clientOrderId = 2, .price = 104, .quantity = 5}}));

    // The exchange turns the ask's modify down, so it is still at 101: the next requote asks
    // again. The bid's modify is still on its way, so it is not repeated.
    maker.onRejected(context, {.agent = 1,
                               .clientOrderId = 2,
                               .request = RequestKind::Modify,
                               .reason = RejectReason::PostOnlyWouldTrade});
    maker.onWakeup(context, 0);
    EXPECT_EQ(context.takeSent(),
              (std::vector<Request>{ModifyOrder{.clientOrderId = 2, .price = 104, .quantity = 5}}));
}

TEST(MarketMakerTest, FairPriceIsARunningAverageOfTrades) {
    MarketMaker maker{kConfig, kReference};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    EXPECT_EQ(maker.fairPrice(), 100.0); // the mid until the first trade

    maker.onTrade(context, {.price = 110, .quantity = 1});
    EXPECT_EQ(maker.fairPrice(), 110.0);
    maker.onTrade(context, {.price = 100, .quantity = 1});
    EXPECT_DOUBLE_EQ(maker.fairPrice(), 108.0); // 20% of the way from 110 to 100
}

TEST(MarketMakerTest, RejectsInvalidConfigs) {
    const auto withChange = [](auto change) {
        MarketMakerConfig config = kConfig;
        change(config);
        return config;
    };
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.riskAversion = 0.0; }), kReference}),
                 std::invalid_argument);
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.intensity = 0.0; }), kReference}),
                 std::invalid_argument);
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.maxInventory = 4; }), kReference}),
                 std::invalid_argument);
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.requoteInterval = 0; }), kReference}),
                 std::invalid_argument);
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.fairValueWeight = 1.5; }), kReference}),
                 std::invalid_argument);
    EXPECT_THROW(
        (MarketMaker{withChange([](auto& c) { c.maxInventory = kMaxQuantity + 1; }), kReference}),
        std::invalid_argument);
}

} // namespace
} // namespace crowdbook

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

// With a threshold its timer leaves its quotes alone until its fair price has moved that far
// from where it set them; without one the same small move requotes it.
TEST(MarketMakerTest, LeavesItsQuotesWhileItsFairPriceMovesLessThanItsThreshold) {
    for (const double threshold : {0.0, 2.0}) {
        MarketMakerConfig config = kConfig;
        config.requoteThreshold = threshold;
        MarketMaker maker{config, kReference};
        FakeContext context;
        maker.onTopOfBook(context, kTop);
        maker.onStart(context); // fair 100, the mid: bid 99, ask 101
        static_cast<void>(context.takeSent());
        context.accept(1);
        context.accept(2);
        const auto tick = [&](Timestamp time, Price traded) {
            context.setNow(time);
            maker.onTrade(context, {.price = traded, .quantity = 1});
            maker.onWakeup(context, 0);
            return context.takeSent().size();
        };
        // The first trade sets the fair price to 101: a tick from where it quoted.
        EXPECT_EQ(tick(100 * kMillisecond, 101), threshold > 0 ? 0U : 2U) << threshold;
        if (threshold > 0) {
            // 101 + 0.2 * (104 - 101) = 101.6, still within 2 of 100: nothing.
            EXPECT_EQ(tick(200 * kMillisecond, 104), 0U);
            // 101.6 + 0.2 * (110 - 101.6) = 103.28: past it, both quotes move.
            EXPECT_EQ(tick(300 * kMillisecond, 110), 2U);
            // And it measures from there: a small move back is left alone.
            EXPECT_EQ(tick(400 * kMillisecond, 103), 0U);
        }
    }
}

// A fill requotes it within its threshold, and a quote missing from the book does at its timer.
TEST(MarketMakerTest, RequotesAfterAFillWhateverItsThreshold) {
    MarketMakerConfig config = kConfig;
    config.requoteThreshold = 5.0;
    MarketMaker maker{config, kReference};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context);
    static_cast<void>(context.takeSent());
    context.accept(1);
    context.accept(2);
    context.fill(1, 5);
    maker.onFilled(context, {});
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{limitOrder(3, Side::Buy, 97, 5),
                                                        ModifyOrder{.clientOrderId = 2,
                                                                    .price = 99,
                                                                    .quantity = 5}}));
    // Its new bid is cancelled by the exchange: the timer puts it back though the fair price
    // has not moved.
    context.accept(3);
    static_cast<void>(context.cancelRest(3));
    context.setNow(100 * kMillisecond);
    maker.onWakeup(context, 0);
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{limitOrder(4, Side::Buy, 97, 5)}));
}

// A partial fill tops the quote up, and a rejected modify is sent again at the next timer, both
// within the threshold.
TEST(MarketMakerTest, TopsUpAndResendsWithinItsThreshold) {
    MarketMakerConfig config = kConfig;
    config.requoteThreshold = 2.0;
    MarketMaker maker{config, kReference};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context);
    static_cast<void>(context.takeSent());
    context.accept(1);
    context.accept(2);
    // As TopsUpAPartlyFilledQuote: short 2, the ask back to 5 lots at 102.
    context.fill(2, 2);
    maker.onFilled(context, {});
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{ModifyOrder{.clientOrderId = 2,
                                                                    .price = 102,
                                                                    .quantity = 5}}));
    // The exchange turns the modify away: the ask stays at 101 with 3 lots, and the next tick
    // of the timer asks again though the fair price has not moved.
    maker.onRejected(context, context.rejectModify(2, RejectReason::PostOnlyWouldTrade));
    context.setNow(100 * kMillisecond);
    maker.onWakeup(context, 0);
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{ModifyOrder{.clientOrderId = 2,
                                                                    .price = 102,
                                                                    .quantity = 5}}));
}

// A ladder level left out at its limit does not keep the threshold from holding the rest.
TEST(MarketMakerTest, HoldsItsQuotesWithALevelLeftOut) {
    MarketMakerConfig config = kConfig;
    config.levels = 2;
    config.maxInventory = 5; // room for the touch's 5 lots a side, not the second level's
    config.requoteThreshold = 2.0;
    MarketMaker maker{config, kReference};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context);
    ASSERT_EQ(context.takeSent().size(), 2U);
    context.accept(1);
    context.accept(2);
    context.setNow(100 * kMillisecond);
    maker.onTrade(context, {.price = 101, .quantity = 1}); // fair 101: within 2 of 100
    maker.onWakeup(context, 0);
    EXPECT_TRUE(context.takeSent().empty());
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

// In a prediction market, whose prices stop at 99, quotes that would go higher stay at the top,
// and the bid leaves room above it for the ask.
TEST(MarketMakerTest, QuotesWithinTheExchangesHighestPrice) {
    MarketMaker maker{kConfig, kReference};
    FakeContext context;
    context.highest = 99;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context);
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{limitOrder(1, Side::Buy, 98, 5),
                                                        limitOrder(2, Side::Sell, 99, 5)}));
}

// With a value belief it quotes around the value as it sees it, whatever trades print.
TEST(MarketMakerTest, QuotesAroundTheValueItSees) {
    MarketMakerConfig config = kConfig;
    config.valueBelief = BeliefConfig{.noise = 0.0};
    const auto value = std::make_shared<Fundamental>(
        FundamentalConfig{.initial = 60.0, .volatility = 0.0}, Random{1, 0});
    MarketMaker maker{config, kReference, value};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onTrade(context, {.price = 110, .quantity = 1});
    maker.onStart(context);
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{limitOrder(1, Side::Buy, 59, 5),
                                                        limitOrder(2, Side::Sell, 61, 5)}));
}

// In a quiet stretch with no interval it pulls its quotes and stays out, then comes back.
TEST(MarketMakerTest, StaysOutOfAQuietStretch) {
    MarketMakerConfig config = kConfig;
    config.quietFrom = kSecond;
    config.quietUntil = 5 * kSecond;
    MarketMaker maker{config, kReference};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context);
    ASSERT_EQ(context.takeSent().size(), 2U);
    context.accept(1);
    context.accept(2);
    context.setNow(kSecond);
    maker.onWakeup(context, 0);
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{CancelOrder{.clientOrderId = 1},
                                                        CancelOrder{.clientOrderId = 2}}));
    context.setNow(3 * kSecond);
    maker.onWakeup(context, 0);
    EXPECT_TRUE(context.takeSent().empty());
    context.setNow(5 * kSecond);
    maker.onWakeup(context, 0);
    EXPECT_EQ(context.takeSent().size(), 2U);
}

// With an interval it requotes only that often in the quiet stretch, whatever the market does.
TEST(MarketMakerTest, QuotesRarelyInAQuietStretch) {
    MarketMakerConfig config = kConfig;
    config.quietFrom = 0;
    config.quietInterval = 2 * kSecond;
    MarketMaker maker{config, kReference};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context);
    ASSERT_EQ(context.takeSent().size(), 2U);
    context.accept(1);
    context.accept(2);
    const auto move = [&](Timestamp time, Price mid) {
        context.setNow(time);
        maker.onTopOfBook(context, {.bid = LevelSummary{.price = mid - 1, .quantity = 1},
                                    .ask = LevelSummary{.price = mid + 1, .quantity = 1}});
        maker.onWakeup(context, 0);
        return context.takeSent().size();
    };
    EXPECT_EQ(move(kSecond, 110), 0U);
    EXPECT_EQ(move(2 * kSecond, 120), 2U); // both quotes move
    EXPECT_EQ(move(3 * kSecond, 130), 0U);
}

TEST(MarketMakerTest, NeedsAValueToQuoteOff) {
    MarketMakerConfig config = kConfig;
    config.valueBelief = BeliefConfig{};
    EXPECT_THROW((MarketMaker{config, kReference}), std::invalid_argument);
}

// A ladder: three levels a side, two ticks apart, each twice the size of the one nearer the
// touch, the first where the one quote would be.
TEST(MarketMakerTest, QuotesALadderOfLevels) {
    MarketMakerConfig config = kConfig;
    config.levels = 3;
    config.levelSpacing = 2;
    config.levelGrowth = 2.0;
    MarketMaker maker{config, kReference};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context);
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{
                                      limitOrder(1, Side::Buy, 99, 5),
                                      limitOrder(2, Side::Sell, 101, 5),
                                      limitOrder(3, Side::Buy, 97, 10),
                                      limitOrder(4, Side::Sell, 103, 10),
                                      limitOrder(5, Side::Buy, 95, 20),
                                      limitOrder(6, Side::Sell, 105, 20),
                                  }));
    for (ClientOrderId id = 1; id <= 6; ++id) {
        context.accept(id);
    }
    // The touch bid fills: long 5, r = 98, so the whole ladder moves down two ticks, a new touch
    // bid at 97 and every other level modified.
    context.fill(1, 5);
    maker.onFilled(context, {});
    EXPECT_EQ(context.takeSent(),
              (std::vector<Request>{
                  limitOrder(7, Side::Buy, 97, 5),
                  ModifyOrder{.clientOrderId = 2, .price = 99, .quantity = 5},
                  ModifyOrder{.clientOrderId = 3, .price = 95, .quantity = 10},
                  ModifyOrder{.clientOrderId = 4, .price = 101, .quantity = 10},
                  ModifyOrder{.clientOrderId = 5, .price = 93, .quantity = 20},
                  ModifyOrder{.clientOrderId = 6, .price = 103, .quantity = 20},
              }));
}

// Levels far apart reach the ends of the exchange's prices and are left out there, never
// wrapping round.
TEST(MarketMakerTest, ALadderStopsAtTheEndsOfThePrices) {
    MarketMakerConfig config = kConfig;
    config.levels = 3;
    config.levelSpacing = kMaxPrice;
    MarketMaker maker{config, kReference};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context);
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{limitOrder(1, Side::Buy, 99, 5),
                                                        limitOrder(2, Side::Sell, 101, 5)}));
}

// A level is quoted only while filling it and every level nearer the touch keeps the position
// within max_inventory: with 20 lots at most, the third level (5 + 10 + 20) is left out.
TEST(MarketMakerTest, ALadderStopsAtItsInventoryLimit) {
    MarketMakerConfig config = kConfig;
    config.maxInventory = 20;
    config.levels = 3;
    config.levelGrowth = 2.0;
    MarketMaker maker{config, kReference};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context);
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{
                                      limitOrder(1, Side::Buy, 99, 5),
                                      limitOrder(2, Side::Sell, 101, 5),
                                      limitOrder(3, Side::Buy, 98, 10),
                                      limitOrder(4, Side::Sell, 102, 10),
                                  }));
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
    EXPECT_THROW((MarketMaker{withChange([](auto& c) {
                                  c.quietFrom = 5;
                                  c.quietUntil = 4;
                              }),
                              kReference}),
                 std::invalid_argument);
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.quietInterval = -1; }), kReference}),
                 std::invalid_argument);
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.levels = 0; }), kReference}),
                 std::invalid_argument);
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.requoteThreshold = -0.5; }), kReference}),
                 std::invalid_argument);
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.levels = 51; }), kReference}),
                 std::invalid_argument);
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.levelSpacing = 0; }), kReference}),
                 std::invalid_argument);
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.levelGrowth = 0.0; }), kReference}),
                 std::invalid_argument);
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.levelGrowth = 1e30; }), kReference}),
                 std::invalid_argument);
    EXPECT_THROW(
        (MarketMaker{withChange([](auto& c) { c.levelSpacing = kMaxPrice + 1; }), kReference}),
        std::invalid_argument);
}

} // namespace
} // namespace crowdbook

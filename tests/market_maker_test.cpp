#include <cmath>
#include <format>
#include <memory>
#include <numeric>
#include <sstream>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/event_log.hpp"
#include "crowdbook/agents/market_maker.hpp"
#include "crowdbook/scenario.hpp"
#include "crowdbook/scenario_file.hpp"
#include "exchange_test_support.hpp"
#include "fake_context.hpp"
#include "test_agents.hpp"

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
// The book once its own 5-lot quotes at 99 and 101 have joined someone else's lot at each.
const TopOfBook kJoinedTop{.bid = LevelSummary{.price = 99, .quantity = 6},
                           .ask = LevelSummary{.price = 101, .quantity = 6}};

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

    // A trade at 103 moves the fair price there, so the quotes move to 102 and 104: the ask first,
    // as a bid at 102 would meet it at 101.
    maker.onTrade(context, {.price = 103, .quantity = 1});
    maker.onWakeup(context, 0);
    ASSERT_EQ(context.takeSent(),
              (std::vector<Request>{ModifyOrder{.clientOrderId = 2, .price = 104, .quantity = 5},
                                    ModifyOrder{.clientOrderId = 1, .price = 102, .quantity = 5}}));

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

// A maker with a threshold comes back after a quiet stretch though its fair price has not moved.
TEST(MarketMakerTest, ComesBackAfterAQuietStretchWhateverItsThreshold) {
    MarketMakerConfig config = kConfig;
    config.quietFrom = kSecond;
    config.quietUntil = 5 * kSecond;
    config.requoteThreshold = 2.0;
    MarketMaker maker{config, kReference};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context);
    ASSERT_EQ(context.takeSent().size(), 2U);
    context.accept(1);
    context.accept(2);
    context.setNow(kSecond);
    maker.onWakeup(context, 0);
    ASSERT_EQ(context.takeSent().size(), 2U); // both cancelled
    context.setNow(5 * kSecond);
    maker.onWakeup(context, 0);
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{limitOrder(3, Side::Buy, 99, 5),
                                                        limitOrder(4, Side::Sell, 101, 5)}));
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
                                      limitOrder(2, Side::Buy, 97, 10),
                                      limitOrder(3, Side::Buy, 95, 20),
                                      limitOrder(4, Side::Sell, 101, 5),
                                      limitOrder(5, Side::Sell, 103, 10),
                                      limitOrder(6, Side::Sell, 105, 20),
                                  }));
    for (ClientOrderId id = 1; id <= 6; ++id) {
        context.accept(id);
    }
    // The touch bid fills: long 5, r = 98, so the whole ladder moves down two ticks, a new touch
    // bid at 97 and every other level modified, the bids before the asks that fall towards them.
    context.fill(1, 5);
    maker.onFilled(context, {});
    EXPECT_EQ(context.takeSent(),
              (std::vector<Request>{
                  limitOrder(7, Side::Buy, 97, 5),
                  ModifyOrder{.clientOrderId = 2, .price = 95, .quantity = 10},
                  ModifyOrder{.clientOrderId = 3, .price = 93, .quantity = 20},
                  ModifyOrder{.clientOrderId = 4, .price = 99, .quantity = 5},
                  ModifyOrder{.clientOrderId = 5, .price = 101, .quantity = 10},
                  ModifyOrder{.clientOrderId = 6, .price = 103, .quantity = 20},
              }));
}

// Levels no ticks apart split the quote into orders at one price: three of 5 lots at 99 and three
// at 101, a queue a large taker meets one after another. A filled one is sent again, at the price
// the position now asks for, and the others follow it there.
TEST(MarketMakerTest, LevelsNoTicksApartSplitTheQuoteIntoOrders) {
    MarketMakerConfig config = kConfig;
    config.levels = 3;
    config.levelSpacing = 0;
    MarketMaker maker{config, kReference};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context);
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{
                                      limitOrder(1, Side::Buy, 99, 5),
                                      limitOrder(2, Side::Buy, 99, 5),
                                      limitOrder(3, Side::Buy, 99, 5),
                                      limitOrder(4, Side::Sell, 101, 5),
                                      limitOrder(5, Side::Sell, 101, 5),
                                      limitOrder(6, Side::Sell, 101, 5),
                                  }));
    for (ClientOrderId id = 1; id <= 6; ++id) {
        context.accept(id);
    }
    // The first bid fills: long 5, r = 98, so the bids move to 97 and the asks to 99, as the
    // one-quote maker's do.
    context.fill(1, 5);
    maker.onFilled(context, {});
    EXPECT_EQ(context.takeSent(),
              (std::vector<Request>{
                  limitOrder(7, Side::Buy, 97, 5),
                  ModifyOrder{.clientOrderId = 2, .price = 97, .quantity = 5},
                  ModifyOrder{.clientOrderId = 3, .price = 97, .quantity = 5},
                  ModifyOrder{.clientOrderId = 4, .price = 99, .quantity = 5},
                  ModifyOrder{.clientOrderId = 5, .price = 99, .quantity = 5},
                  ModifyOrder{.clientOrderId = 6, .price = 99, .quantity = 5},
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
                                      limitOrder(2, Side::Buy, 98, 10),
                                      limitOrder(3, Side::Sell, 101, 5),
                                      limitOrder(4, Side::Sell, 102, 10),
                                  }));
}

// A maker that reads order flow and sees a value of 100 that never moves, so that its fair price
// is 100 plus what the flow has moved it.
MarketMakerConfig flowReader(double impact) {
    MarketMakerConfig config = kConfig;
    config.valueBelief = BeliefConfig{.noise = 0.0};
    config.flowImpact = impact;
    return config;
}

std::shared_ptr<Fundamental> stillValue() {
    return std::make_shared<Fundamental>(FundamentalConfig{.initial = 100.0, .volatility = 0.0},
                                         Random{1, 0});
}

// Each lot of aggressive volume moves the fair price flow_impact ticks, up for a buy and down for
// a sell; an auction's trades not at all. With a memory the impact falls to 1/e after it; without
// one it stays.
TEST(MarketMakerTest, OrderFlowMovesItsFairPrice) {
    MarketMakerConfig config = kConfig;
    config.flowImpact = 0.5;
    config.flowMemory = 10 * kSecond;
    MarketMaker maker{config, kReference};
    FakeContext context;
    maker.onTrade(context, {.price = 100, .quantity = 4, .aggressorSide = Side::Buy});
    EXPECT_DOUBLE_EQ(maker.flowImpact(0), 2.0);
    EXPECT_NEAR(maker.flowImpact(10 * kSecond), 2.0 / std::exp(1.0), 1e-12);
    context.setNow(10 * kSecond);
    maker.onTrade(context, {.price = 100, .quantity = 2, .aggressorSide = Side::Sell});
    EXPECT_NEAR(maker.flowImpact(10 * kSecond), 2.0 / std::exp(1.0) - 1.0, 1e-12);
    maker.onTrade(context,
                  {.price = 100, .quantity = 50, .aggressorSide = Side::Buy, .auction = true});
    EXPECT_NEAR(maker.flowImpact(10 * kSecond), 2.0 / std::exp(1.0) - 1.0, 1e-12);

    config.flowMemory.reset();
    MarketMaker lasting{config, kReference};
    lasting.onTrade(context, {.price = 100, .quantity = 4, .aggressorSide = Side::Buy});
    EXPECT_DOUBLE_EQ(lasting.flowImpact(10 * kSecond + 3600 * kSecond), 2.0);
}

// It quotes around its anchor plus the impact: a buy of 5 lots at an impact of 1 tick a lot moves
// its quotes from 99/101 to 104/106.
TEST(MarketMakerTest, QuotesAroundItsAnchorPlusTheFlowsImpact) {
    MarketMaker maker{flowReader(1.0), kReference, stillValue()};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onTrade(context, {.price = 101, .quantity = 5, .aggressorSide = Side::Buy});
    maker.onStart(context);
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{limitOrder(1, Side::Buy, 104, 5),
                                                        limitOrder(2, Side::Sell, 106, 5)}));
}

// When its fair price has moved past the far side of the book by more than take_through, it takes
// the stale quotes there with an immediate-or-cancel order, at its fair price less the threshold,
// for as much as its inventory limit leaves room for, and quotes as if that filled.
TEST(MarketMakerTest, TakesQuotesItsFairPriceHasMovedPastUpToItsLimit) {
    MarketMakerConfig config = flowReader(1.0);
    config.maxInventory = 10;
    config.takeThrough = 2.0;
    MarketMaker maker{config, kReference, stillValue()};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    // Fair 105: 105 - 2 = 103 is past the ask of 101. Counting the take as filled, it is long 10,
    // at its limit: no bid, and the ask at ceil(105 - 10 * 0.4 + 0.845) = 102.
    maker.onTrade(context, {.price = 101, .quantity = 5, .aggressorSide = Side::Buy});
    maker.onStart(context);
    EXPECT_EQ(context.takeSent(),
              (std::vector<Request>{limitOrder(1, Side::Buy, 103, 10, TimeInForce::ImmediateOrCancel),
                                    limitOrder(2, Side::Sell, 102, 5)}));
    // 4 lots fill and the rest is cancelled: it takes again, but only the 6 lots left to its
    // limit. Its own ask at 102 is in the take's way, and the exchange would stop the take there,
    // so it cancels the ask first and quotes it afresh after.
    context.accept(1);
    context.accept(2);
    context.fill(1, 4);
    static_cast<void>(context.cancelRest(1));
    context.setNow(100 * kMillisecond);
    maker.onWakeup(context, 0);
    EXPECT_EQ(context.takeSent(),
              (std::vector<Request>{CancelOrder{.clientOrderId = 2},
                                    limitOrder(3, Side::Buy, 103, 6, TimeInForce::ImmediateOrCancel),
                                    limitOrder(4, Side::Sell, 102, 5)}));
    // At its limit, with nothing open, it takes nothing more.
    context.accept(3);
    context.fill(3, 6);
    context.setNow(200 * kMillisecond);
    maker.onWakeup(context, 0);
    for (const Request& request : context.takeSent()) {
        const auto* order = std::get_if<NewOrder>(&request);
        EXPECT_FALSE(order != nullptr && order->side == Side::Buy);
    }
}

TEST(MarketMakerTest, TakesTheBidSideToo) {
    MarketMakerConfig config = flowReader(1.0);
    config.maxInventory = 10;
    config.takeThrough = 2.0;
    MarketMaker maker{config, kReference, stillValue()};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    // Fair 95: 95 + 2 = 97 is below the bid of 99.
    maker.onTrade(context, {.price = 99, .quantity = 5, .aggressorSide = Side::Sell});
    maker.onStart(context);
    const std::vector<Request> sent = context.takeSent();
    ASSERT_FALSE(sent.empty());
    EXPECT_EQ(sent[0], (Request{limitOrder(1, Side::Sell, 97, 10, TimeInForce::ImmediateOrCancel)}));
}

// Its take is no bigger than the room its open orders leave: a bid of 5 lots already resting
// leaves 5 of a limit of 10.
TEST(MarketMakerTest, ATakeLeavesRoomForItsOpenOrders) {
    MarketMakerConfig config = flowReader(1.0);
    config.maxInventory = 10;
    config.takeThrough = 2.0;
    MarketMaker maker{config, kReference, stillValue()};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context); // bid 99 and ask 101, 5 lots each
    static_cast<void>(context.takeSent());
    context.accept(1);
    context.accept(2);
    maker.onTopOfBook(context, kJoinedTop);
    maker.onTrade(context, {.price = 101, .quantity = 5, .aggressorSide = Side::Buy});
    context.setNow(100 * kMillisecond);
    maker.onWakeup(context, 0);
    const std::vector<Request> sent = context.takeSent();
    ASSERT_GE(sent.size(), 2U);
    EXPECT_EQ(sent[0], (Request{CancelOrder{.clientOrderId = 2}})); // its ask at 101, in the way
    EXPECT_EQ(sent[1], (Request{limitOrder(3, Side::Buy, 103, 5, TimeInForce::ImmediateOrCancel)}));
}

// A take is priced at the fair price less the threshold rounded away from the book's far side,
// so that it never pays more than that: floor for a buy, ceil for a sell.
TEST(MarketMakerTest, RoundsATakesPriceInItsOwnFavour) {
    MarketMakerConfig config = flowReader(0.5);
    config.maxInventory = 10;
    config.takeThrough = 1.0;
    MarketMaker buyer{config, kReference, stillValue()};
    FakeContext context;
    buyer.onTopOfBook(context, kTop);
    // Fair 104.5: 103.5 is past the ask of 101, and it buys at 103.
    buyer.onTrade(context, {.price = 101, .quantity = 9, .aggressorSide = Side::Buy});
    buyer.onStart(context);
    EXPECT_EQ(context.takeSent().front(),
              (Request{limitOrder(1, Side::Buy, 103, 10, TimeInForce::ImmediateOrCancel)}));

    MarketMaker seller{config, kReference, stillValue()};
    FakeContext other;
    seller.onTopOfBook(other, kTop);
    // Fair 95.5: 96.5 is below the bid of 99, and it sells at 97.
    seller.onTrade(other, {.price = 99, .quantity = 9, .aggressorSide = Side::Sell});
    seller.onStart(other);
    EXPECT_EQ(other.takeSent().front(),
              (Request{limitOrder(1, Side::Sell, 97, 10, TimeInForce::ImmediateOrCancel)}));
}

// Exactly the threshold past the far side is not past it.
TEST(MarketMakerTest, TakesOnlyMoreThanItsThresholdPastTheBook) {
    MarketMakerConfig config = flowReader(1.0);
    config.takeThrough = 2.0;
    MarketMaker maker{config, kReference, stillValue()};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    // Fair 103: 103 - 2 is the ask of 101 itself.
    maker.onTrade(context, {.price = 101, .quantity = 3, .aggressorSide = Side::Buy});
    maker.onStart(context);
    for (const Request& request : context.takeSent()) {
        const auto* order = std::get_if<NewOrder>(&request);
        ASSERT_NE(order, nullptr);
        EXPECT_EQ(order->timeInForce, TimeInForce::GoodTillCancel);
    }
}

// Long 5, a sell take may go to 10 short: 15 lots, less the 5 its ask has open; and it quotes as
// if the take filled, 5 short.
TEST(MarketMakerTest, ASellTakeCountsItsPositionAndQuotesAsIfItFilled) {
    MarketMakerConfig config = flowReader(1.0);
    config.maxInventory = 10;
    config.takeThrough = 2.0;
    MarketMaker maker{config, kReference, stillValue()};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context); // bid 99 and ask 101
    static_cast<void>(context.takeSent());
    context.accept(1);
    context.accept(2);
    context.fill(1, 5); // long 5
    maker.onFilled(context, {.clientOrderId = 1, .side = Side::Buy, .quantity = 5});
    static_cast<void>(context.takeSent());
    // Fair 95: 97 is below the bid of 99; long 5 with an ask of 5 open, it sells 10 at 97.
    maker.onTrade(context, {.price = 99, .quantity = 5, .aggressorSide = Side::Sell});
    context.setNow(100 * kMillisecond);
    maker.onWakeup(context, 0);
    const std::vector<Request> sent = context.takeSent();
    const auto take = std::ranges::find_if(sent, [](const Request& request) {
        const auto* order = std::get_if<NewOrder>(&request);
        return order != nullptr && order->timeInForce == TimeInForce::ImmediateOrCancel;
    });
    ASSERT_NE(take, sent.end());
    EXPECT_EQ(*take, (Request{limitOrder(4, Side::Sell, 97, 10, TimeInForce::ImmediateOrCancel)}));
    // Its bid at 97, which the take would reach, is cancelled first. Then, as if 5 short:
    // r = 95 + 5 * 0.4 = 97, a new bid at floor(97 - 0.845) = 96 and the ask to ceil(97.845) = 98,
    // which could take it to 10 short and no further.
    EXPECT_EQ(sent, (std::vector<Request>{
                        CancelOrder{.clientOrderId = 3},
                        limitOrder(4, Side::Sell, 97, 10, TimeInForce::ImmediateOrCancel),
                        limitOrder(5, Side::Buy, 96, 5),
                        ModifyOrder{.clientOrderId = 2, .price = 98, .quantity = 5}}));
}

// Its own take prints as aggressive volume, but it learns nothing from it. The take's fill reaches
// it before the trade, as the simulation delivers them, and the requote the fill brings sees the
// fair price where it was; the trade then adds nothing for the take's lots, only for others'.
TEST(MarketMakerTest, LearnsNothingFromItsOwnTakes) {
    MarketMakerConfig config = flowReader(1.0);
    config.maxInventory = 10;
    config.takeThrough = 2.0;
    MarketMaker maker{config, kReference, stillValue()};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onTrade(context, {.price = 101, .quantity = 5, .aggressorSide = Side::Buy});
    maker.onStart(context); // takes with order 1
    static_cast<void>(context.takeSent());
    context.accept(1);
    context.fill(1, 3);
    maker.onFilled(context, {.clientOrderId = 1, .side = Side::Buy, .quantity = 3});
    EXPECT_DOUBLE_EQ(maker.flowImpact(0), 5.0);
    maker.onTrade(context, {.price = 101, .quantity = 3, .aggressorSide = Side::Buy});
    EXPECT_DOUBLE_EQ(maker.flowImpact(0), 5.0);
    maker.onTrade(context, {.price = 101, .quantity = 2, .aggressorSide = Side::Buy});
    EXPECT_DOUBLE_EQ(maker.flowImpact(0), 7.0); // another trader's buy counts
}

// A quote of its own that a modify on its way is moving into the take's reach is cancelled too,
// though the book still has it out of reach.
TEST(MarketMakerTest, CancelsAQuoteAModifyIsMovingIntoTheTakesWay) {
    MarketMakerConfig config = flowReader(1.0);
    config.maxInventory = 10;
    config.takeThrough = 2.0;
    MarketMaker maker{config, kReference, stillValue()};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    // Fair 103, exactly the threshold past the ask of 101: no take, and quotes at 102 and 104,
    // acknowledged.
    maker.onTrade(context, {.price = 101, .quantity = 3, .aggressorSide = Side::Buy});
    maker.onStart(context);
    static_cast<void>(context.takeSent());
    context.accept(1);
    context.accept(2);
    // Fair 100: the ask is modified to 101, the modify still on its way.
    maker.onTrade(context, {.price = 101, .quantity = 3, .aggressorSide = Side::Sell});
    context.setNow(100 * kMillisecond);
    maker.onWakeup(context, 0);
    static_cast<void>(context.takeSent());
    // Fair 105: a take at 103, which the ask at 101 would stop, though the book has it at 104.
    maker.onTrade(context, {.price = 101, .quantity = 5, .aggressorSide = Side::Buy});
    context.setNow(200 * kMillisecond);
    maker.onWakeup(context, 0);
    const std::vector<Request> sent = context.takeSent();
    ASSERT_GE(sent.size(), 2U);
    EXPECT_EQ(sent[0], (Request{CancelOrder{.clientOrderId = 2}}));
    const auto* take = std::get_if<NewOrder>(&sent[1]);
    ASSERT_NE(take, nullptr);
    EXPECT_EQ(take->timeInForce, TimeInForce::ImmediateOrCancel);
}

// Whatever its requote threshold, it quotes again in place of a quote it cancelled for a take.
TEST(MarketMakerTest, ReplacesAQuoteItCancelledForATakeWhateverItsThreshold) {
    MarketMakerConfig config = flowReader(1.0);
    config.maxInventory = 10;
    config.takeThrough = 2.0;
    config.requoteThreshold = 50.0;
    MarketMaker maker{config, kReference, stillValue()};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context); // 99 and 101
    static_cast<void>(context.takeSent());
    context.accept(1);
    context.accept(2);
    maker.onTopOfBook(context, kJoinedTop);
    maker.onTrade(context, {.price = 101, .quantity = 5, .aggressorSide = Side::Buy});
    context.setNow(100 * kMillisecond);
    maker.onWakeup(context, 0);
    const std::vector<Request> sent = context.takeSent();
    const bool askAgain = std::ranges::any_of(sent, [](const Request& request) {
        const auto* order = std::get_if<NewOrder>(&request);
        return order != nullptr && order->side == Side::Sell &&
               order->timeInForce == TimeInForce::GoodTillCancel;
    });
    EXPECT_TRUE(askAgain);
}

// When the rest of a take is cancelled it quotes afresh, no longer as if all of it filled.
TEST(MarketMakerTest, QuotesAfreshWhenATakeEnds) {
    MarketMakerConfig config = flowReader(1.0);
    config.maxInventory = 10;
    config.takeThrough = 2.0;
    MarketMaker maker{config, kReference, stillValue()};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onTrade(context, {.price = 101, .quantity = 5, .aggressorSide = Side::Buy});
    maker.onStart(context); // a take of 10 and an ask at 102, as if long 10
    static_cast<void>(context.takeSent());
    context.accept(1);
    context.accept(2);
    // Nothing fills, and the fair price comes back to 102, within the threshold.
    maker.onTrade(context, {.price = 101, .quantity = 3, .aggressorSide = Side::Sell});
    maker.onCancelled(context, context.cancelRest(1));
    // Flat: bid floor(102 - 0.845) = 101 and the ask to ceil(102.845) = 103.
    EXPECT_EQ(context.takeSent(),
              (std::vector<Request>{limitOrder(3, Side::Buy, 101, 5),
                                    ModifyOrder{.clientOrderId = 2, .price = 103, .quantity = 5}}));
}

// The takes among the requests sent.
std::ptrdiff_t takesIn(const std::vector<Request>& sent) {
    return std::ranges::count_if(sent, [](const Request& request) {
        const auto* order = std::get_if<NewOrder>(&request);
        return order != nullptr && order->timeInForce == TimeInForce::ImmediateOrCancel;
    });
}

// When a take ends with its fair price still past someone else's quote, as when the take stopped
// short of it, it takes again only on its timer: takes never follow one another without time
// passing.
TEST(MarketMakerTest, TakesAgainOnlyOnItsTimer) {
    MarketMakerConfig config = flowReader(1.0);
    config.maxInventory = 10;
    config.takeThrough = 2.0;
    MarketMaker maker{config, kReference, stillValue()};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onTrade(context, {.price = 101, .quantity = 5, .aggressorSide = Side::Buy});
    maker.onStart(context); // a take of 10 at 103 and an ask at 102, as if long 10
    static_cast<void>(context.takeSent());
    context.accept(1);
    context.accept(2);
    maker.onCancelled(context, context.cancelRest(1)); // fair 105 is still past the ask of 101
    EXPECT_EQ(takesIn(context.takeSent()), 0);
    context.setNow(100 * kMillisecond);
    maker.onWakeup(context, 0);
    EXPECT_EQ(takesIn(context.takeSent()), 1);
}

struct MakerOrders {
    int takes = 0;      // immediate-or-cancel orders it sent
    int selfTrades = 0; // its orders cancelled for meeting its own
};

// Runs a scenario and counts the orders of the first agent of its first group.
MakerOrders firstMakersOrders(const Scenario& scenario) {
    std::ostringstream log;
    CsvEventLog sink{log, {"new", "cancelled"}};
    const RunResult result = runScenario(scenario, AgentRegistry::withBuiltIns(), &sink);
    const std::string maker = std::to_string(result.groups[0].agents[0]);
    MakerOrders orders;
    std::string line;
    std::istringstream rows{log.str()};
    std::getline(rows, line); // time,kind,agent,...,time_in_force (8th),...,reason (15th)
    while (std::getline(rows, line)) {
        std::vector<std::string> fields;
        std::istringstream split{line};
        for (std::string field; std::getline(split, field, ',');) {
            fields.push_back(field);
        }
        fields.resize(15);
        if (fields[2] != maker) {
            continue;
        }
        orders.takes += fields[1] == "new" && fields[7] == "immediate-or-cancel" ? 1 : 0;
        orders.selfTrades += fields[1] == "cancelled" && fields[14] == "self-trade" ? 1 : 0;
    }
    return orders;
}

// In a whole market, its takes are never stopped by its own quotes.
TEST(MarketMakerTest, ItsTakesNeverMeetItsOwnQuotes) {
    const Scenario scenario = parseScenario(R"(
seed = 7
duration = "300s"
reference_price = 1000

[fundamental]
volatility = 2.0
step = "100ms"

[[agents]]
type = "market_maker"
name = "setter"
quote_size = 2
max_inventory = 40
volatility = 1.0
intensity = 1.0
quote_value = true
lag = "20s"
flow_impact = 0.3
flow_memory = "60s"
take_through = 1

[[agents]]
type = "zero_intelligence"
count = 10
limit_rate = 1.0
market_rate = 0.3

[[agents]]
type = "informed"
count = 3
interval = "1s"
)");
    const MakerOrders orders = firstMakersOrders(scenario);
    EXPECT_GT(orders.takes, 0);
    EXPECT_EQ(orders.selfTrades, 0);
}

// In a whole market whose price jumps, as a maker quoting off a value it sees 20 seconds late
// finds, a maker that reads no flow never has a quote cancelled for meeting its own, one level or
// a ladder: one side moves before the other, the asks first when its bid would reach them.
TEST(MarketMakerTest, ItsQuotesNeverMeetEachOther) {
    for (const std::string_view ladder :
         {"levels = 1", "levels = 2", "levels = 3\nlevel_spacing = 1",
          "levels = 3\nlevel_spacing = 3"}) {
        const Scenario scenario = parseScenario(std::format(R"(
seed = 7
duration = "300s"
reference_price = 1000

[fundamental]
volatility = 2.0
step = "100ms"

[[agents]]
type = "market_maker"
name = "setter"
quote_size = 2
max_inventory = 40
volatility = 1.0
intensity = 1.0
quote_value = true
lag = "20s"
{}

[[agents]]
type = "zero_intelligence"
count = 10
limit_rate = 1.0
market_rate = 0.3

[[agents]]
type = "informed"
count = 3
interval = "1s"
)",
                                                          ladder));
        EXPECT_EQ(firstMakersOrders(scenario).selfTrades, 0) << ladder;
    }
}

// When the best price on the far side holds only its own quote it has nothing to take there.
TEST(MarketMakerTest, TakesNothingWhereTheBookHoldsOnlyItsOwnQuote) {
    MarketMakerConfig config = flowReader(1.0);
    config.maxInventory = 10;
    config.takeThrough = 2.0;
    MarketMaker maker{config, kReference, stillValue()};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context); // bid 99 and ask 101, 5 lots each
    static_cast<void>(context.takeSent());
    context.accept(1);
    context.accept(2);
    maker.onTopOfBook(context, {.bid = LevelSummary{.price = 99, .quantity = 5},
                                .ask = LevelSummary{.price = 101, .quantity = 5}});
    maker.onTrade(context, {.price = 101, .quantity = 5, .aggressorSide = Side::Buy});
    context.setNow(100 * kMillisecond);
    maker.onWakeup(context, 0);
    for (const Request& request : context.takeSent()) {
        const auto* order = std::get_if<NewOrder>(&request);
        EXPECT_FALSE(order != nullptr && order->timeInForce == TimeInForce::ImmediateOrCancel);
    }
}

// A fill of its quote at the best price reaches it before the book that follows it: the lots it
// lost there are no one else's.
TEST(MarketMakerTest, TakesNothingOnAFillOfItsOwnQuoteAtTheBestPrice) {
    MarketMakerConfig config = flowReader(1.0);
    config.maxInventory = 10;
    config.takeThrough = 2.0;
    MarketMaker maker{config, kReference, stillValue()};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context); // bid 99 and ask 101, 5 lots each
    static_cast<void>(context.takeSent());
    context.accept(1);
    context.accept(2);
    maker.onTopOfBook(context, {.bid = LevelSummary{.price = 99, .quantity = 5},
                                .ask = LevelSummary{.price = 101, .quantity = 5}});
    maker.onTrade(context, {.price = 101, .quantity = 5, .aggressorSide = Side::Buy}); // fair 105
    context.setNow(100 * kMillisecond);
    maker.onWakeup(context, 0);
    ASSERT_EQ(takesIn(context.takeSent()), 0);
    // Someone lifts 2 lots of its ask while the modify moving it is on its way.
    const OwnOrder ask = *context.ledger().find(2);
    context.fill(2, 2);
    maker.onFilled(context, {.clientOrderId = 2,
                             .orderId = ask.orderId,
                             .side = Side::Sell,
                             .price = 101,
                             .quantity = 2,
                             .leavesQuantity = 3});
    EXPECT_EQ(takesIn(context.takeSent()), 0);
}

// A quote it is cancelling is still its own in the book until the cancel arrives.
TEST(MarketMakerTest, CountsAQuoteWithACancelOnItsWayAsItsOwn) {
    MarketMakerConfig config = flowReader(1.0);
    config.maxInventory = 10;
    config.takeThrough = 2.0;
    MarketMaker maker{config, kReference, stillValue()};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    maker.onStart(context); // bid 99 and ask 101, 5 lots each
    static_cast<void>(context.takeSent());
    context.accept(1);
    context.accept(2);
    maker.onTopOfBook(context, kJoinedTop);
    maker.onTrade(context, {.price = 101, .quantity = 5, .aggressorSide = Side::Buy}); // fair 105
    context.setNow(100 * kMillisecond);
    maker.onWakeup(context, 0); // cancels its ask at 101 and takes at 103
    const std::vector<Request> sent = context.takeSent();
    ASSERT_EQ(sent[0], (Request{CancelOrder{.clientOrderId = 2}}));
    ASSERT_EQ(takesIn(sent), 1);
    // The take buys the other lot at 101 and ends; the cancel of its ask is slower.
    context.accept(3);
    context.fill(3, 1);
    maker.onFilled(context, {.clientOrderId = 3,
                             .orderId = context.ledger().find(3)->orderId,
                             .side = Side::Buy,
                             .price = 101,
                             .quantity = 1,
                             .leavesQuantity = 4,
                             .liquidity = Liquidity::Taker});
    maker.onCancelled(context, context.cancelRest(3));
    maker.onTopOfBook(context, {.bid = kJoinedTop.bid,
                                .ask = LevelSummary{.price = 101, .quantity = 5}});
    static_cast<void>(context.takeSent());
    context.setNow(200 * kMillisecond);
    maker.onWakeup(context, 0);
    EXPECT_EQ(takesIn(context.takeSent()), 0);
}

// Counts the requests reaching the exchange, and the takes among them, and stops the run past a
// limit.
class RequestLimit final : public EventSink {
public:
    explicit RequestLimit(int limit) : limit_{limit} {}
    void onRequest(Timestamp /*time*/, AgentId /*agent*/, const Request& request) override {
        if (++requests_ > limit_) {
            throw std::runtime_error("too many requests");
        }
        const auto* order = std::get_if<NewOrder>(&request);
        if (order != nullptr && order->type == OrderType::Limit &&
            order->timeInForce == TimeInForce::ImmediateOrCancel) {
            ++takes;
        }
    }
    void onEvent(Timestamp /*time*/, const Event& /*event*/) override {}

    int takes = 0;

private:
    int limit_;
    int requests_ = 0;
};

// On a real exchange, where a take's fill arrives before its trade: a buy of 8 lots moves its fair
// price past the asks, and the take, cancelling its own ask, ends; the requote after it must not
// take again at once, nor take its own ask on later ticks once no one else's is left in reach.
TEST(MarketMakerTest, ATakeOnAnExchangeEndsAndIsNotRepeatedAgainstItself) {
    Simulation simulation{1};
    RequestLimit sink{1000};
    simulation.setEventSink(&sink);
    auto& stale = test::add<test::RecordingAgent>(simulation, {});
    stale.startHook = [](AgentContext& context) {
        context.submitLimit(Side::Sell, 103, 12);
        context.submitLimit(Side::Sell, 105, 10);
        context.submitLimit(Side::Buy, 90, 20);
    };
    MarketMakerConfig config = flowReader(1.0);
    config.takeThrough = 1.0;
    static_cast<void>(test::add<MarketMaker>(simulation, {}, config, kReference, stillValue()));
    auto& buyer = test::add<test::RecordingAgent>(simulation, {});
    buyer.startHook = [](AgentContext& context) { context.wakeAt(50 * kMillisecond, 0); };
    buyer.wakeupHook = [](AgentContext& context, std::uint64_t) {
        context.submitMarket(Side::Buy, 8);
    };
    EXPECT_NO_THROW(simulation.runUntil(kSecond));
    EXPECT_GT(sink.takes, 0);
    EXPECT_LE(sink.takes, 2) << "it takes on every tick";
}

// Within the threshold it takes nothing and quotes as usual.
TEST(MarketMakerTest, TakesNothingWithinItsThreshold) {
    MarketMakerConfig config = flowReader(1.0);
    config.takeThrough = 2.0;
    MarketMaker maker{config, kReference, stillValue()};
    FakeContext context;
    maker.onTopOfBook(context, kTop);
    // Fair 102: 102 - 2 = 100 is not past the ask of 101.
    maker.onTrade(context, {.price = 101, .quantity = 2, .aggressorSide = Side::Buy});
    maker.onStart(context);
    EXPECT_EQ(context.takeSent(), (std::vector<Request>{limitOrder(1, Side::Buy, 101, 5),
                                                        limitOrder(2, Side::Sell, 103, 5)}));
}

// A maker's profit a lot traded, over seeds, in a market whose value walks, against `takers`;
// it sees the value 20 s late, as Glosten and Milgrom's dealer knows less than the informed.
std::vector<double> profitPerLot(const std::string& takers, double impact) {
    std::vector<double> profits;
    for (int seed = 1; seed <= 40; ++seed) {
        const Scenario scenario = parseScenario(std::format(R"(
seed = {}
duration = "600s"
reference_price = 1000

[fundamental]
volatility = 1.0
step = "100ms"

[[agents]]
type = "market_maker"
quote_size = 2
max_inventory = 40
volatility = 1.0
intensity = 1.0
quote_value = true
lag = "20s"
{}

{})",
                                                            seed,
                                                            impact > 0.0
                                                                ? std::format("flow_impact = {}\n"
                                                                              "flow_memory = \"20s\"",
                                                                              impact)
                                                                : std::string{},
                                                            takers));
        const RunResult result = runScenario(scenario, AgentRegistry::withBuiltIns(), nullptr);
        const GroupResult& maker = result.groups[0];
        profits.push_back(static_cast<double>(maker.pnl) /
                          static_cast<double>(std::max<Quantity>(maker.traded, 1)));
    }
    return profits;
}

struct Gain {
    double mean = 0.0;    // with flow reading less without, seed by seed, a lot
    double error = 0.0;   // its standard error
    double without = 0.0; // the profit a lot without
};

Gain gain(const std::string& takers) {
    const std::vector<double> without = profitPerLot(takers, 0.0);
    const std::vector<double> with = profitPerLot(takers, 0.1);
    std::vector<double> differences;
    for (std::size_t i = 0; i < with.size(); ++i) {
        differences.push_back(with[i] - without[i]);
    }
    const auto n = static_cast<double>(differences.size());
    const double mean = std::accumulate(differences.begin(), differences.end(), 0.0) / n;
    double squares = 0.0;
    for (const double difference : differences) {
        squares += (difference - mean) * (difference - mean);
    }
    return {.mean = mean,
            .error = std::sqrt(squares / (n - 1.0) / n),
            .without = std::accumulate(without.begin(), without.end(), 0.0) / n};
}

// Glosten and Milgrom: against takers who know the value, a maker who reads their flow loses
// less a lot than one who does not. Against takers who know nothing it keeps nearly all it earns
// from them: over 40 seeds it gives up about 3% (+1.44 a lot against +1.49), moving its quotes
// on flow that says nothing, so the bound is 5% of what it earns without.
TEST(MarketMakerTest, ReadingFlowLosesLessToTheInformedAndLittleToNoise) {
    const Gain informed = gain(R"([[agents]]
type = "informed"
count = 4
interval = "1s")");
    EXPECT_GT(informed.mean - 2.0 * informed.error, 0.0)
        << informed.mean << " ± " << informed.error;
    const Gain noise = gain(R"([[agents]]
type = "zero_intelligence"
count = 4
limit_rate = 0.0
market_rate = 0.5)");
    EXPECT_GT(noise.without, 0.0);
    EXPECT_GT(noise.mean + 2.0 * noise.error, -0.05 * noise.without)
        << noise.mean << " ± " << noise.error << " of " << noise.without;
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
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.levelSpacing = -1; }), kReference}),
                 std::invalid_argument);
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.levelGrowth = 0.0; }), kReference}),
                 std::invalid_argument);
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.levelGrowth = 1e30; }), kReference}),
                 std::invalid_argument);
    EXPECT_THROW(
        (MarketMaker{withChange([](auto& c) { c.levelSpacing = kMaxPrice + 1; }), kReference}),
        std::invalid_argument);
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.flowImpact = -0.1; }), kReference}),
                 std::invalid_argument);
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.flowMemory = 0; }), kReference}),
                 std::invalid_argument);
    EXPECT_THROW((MarketMaker{withChange([](auto& c) { c.takeThrough = -1.0; }), kReference}),
                 std::invalid_argument);
}

} // namespace
} // namespace crowdbook

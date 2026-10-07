#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/order_book.hpp"
#include "order_test_support.hpp"

namespace crowdbook {
namespace {

using test::cancelled;
using test::filled;
using test::limit;
using test::market;
using test::rejected;
using test::resting;

constexpr AgentId kAlice = 1;
constexpr AgentId kBob = 2;
constexpr AgentId kCarol = 3;

// Every test ends by checking the book's internal structure.
class OrderBookTest : public ::testing::Test {
protected:
    void TearDown() override { EXPECT_EQ(book_.audit(), std::nullopt); }

    OrderBook book_;
    std::vector<Fill> fills_;
};

TEST_F(OrderBookTest, StartsEmpty) {
    EXPECT_EQ(book_.bestBid(), std::nullopt);
    EXPECT_EQ(book_.bestAsk(), std::nullopt);
    EXPECT_EQ(book_.orderCount(), 0U);
    EXPECT_TRUE(book_.depth(Side::Buy).empty());
    EXPECT_TRUE(book_.depth(Side::Sell).empty());
}

TEST_F(OrderBookTest, LimitOrderThatDoesNotCrossRests) {
    EXPECT_EQ(book_.submit(limit(1, kAlice, Side::Buy, 99, 5), fills_), resting(5));
    EXPECT_EQ(book_.submit(limit(2, kBob, Side::Sell, 101, 3), fills_), resting(3));

    EXPECT_TRUE(fills_.empty());
    EXPECT_EQ(book_.bestBid(), Price{99});
    EXPECT_EQ(book_.bestAsk(), Price{101});
    EXPECT_EQ(book_.find(1), (RestingOrder{.id = 1,
                                           .owner = kAlice,
                                           .side = Side::Buy,
                                           .price = 99,
                                           .remaining = 5}));
}

TEST_F(OrderBookTest, TradesAtTheRestingOrdersPrice) {
    book_.submit(limit(1, kBob, Side::Sell, 101, 5), fills_);

    EXPECT_EQ(book_.submit(limit(2, kAlice, Side::Buy, 103, 3), fills_), filled(3));
    EXPECT_EQ(fills_, (std::vector<Fill>{{.makerOrderId = 1,
                                          .takerOrderId = 2,
                                          .makerOwner = kBob,
                                          .takerOwner = kAlice,
                                          .takerSide = Side::Buy,
                                          .price = 101,
                                          .quantity = 3,
                                          .makerRemaining = 2}}));
    EXPECT_EQ(book_.depth(Side::Sell),
              (std::vector<LevelSummary>{{.price = 101, .quantity = 2, .orderCount = 1}}));
}

TEST_F(OrderBookTest, LimitOrderNeverTradesBeyondItsPrice) {
    book_.submit(limit(1, kBob, Side::Sell, 101, 3), fills_);
    book_.submit(limit(2, kBob, Side::Sell, 102, 3), fills_);
    book_.submit(limit(3, kBob, Side::Sell, 104, 3), fills_);

    EXPECT_EQ(book_.submit(limit(4, kAlice, Side::Buy, 102, 10), fills_), resting(4, 6));
    ASSERT_EQ(fills_.size(), 2U);
    EXPECT_EQ(fills_[0].price, 101);
    EXPECT_EQ(fills_[1].price, 102);
    EXPECT_EQ(book_.bestBid(), Price{102}); // the unfilled 4 lots rest at the limit
    EXPECT_EQ(book_.bestAsk(), Price{104}); // beyond the limit, never touched
}

TEST_F(OrderBookTest, EarlierOrderAtTheSamePriceTradesFirst) {
    book_.submit(limit(1, kBob, Side::Sell, 100, 2), fills_);
    book_.submit(limit(2, kCarol, Side::Sell, 100, 2), fills_);

    EXPECT_EQ(book_.submit(market(3, kAlice, Side::Buy, 3), fills_), filled(3));
    ASSERT_EQ(fills_.size(), 2U);
    EXPECT_EQ(fills_[0].makerOrderId, 1U);
    EXPECT_EQ(fills_[0].quantity, 2);
    EXPECT_EQ(fills_[1].makerOrderId, 2U);
    EXPECT_EQ(fills_[1].quantity, 1);
    EXPECT_EQ(book_.find(2), (RestingOrder{.id = 2,
                                           .owner = kCarol,
                                           .side = Side::Sell,
                                           .price = 100,
                                           .remaining = 1}));
}

TEST_F(OrderBookTest, BetterPriceTradesBeforeEarlierOrder) {
    book_.submit(limit(1, kBob, Side::Sell, 101, 2), fills_);
    book_.submit(limit(2, kCarol, Side::Sell, 100, 2), fills_);

    EXPECT_EQ(book_.submit(market(3, kAlice, Side::Buy, 1), fills_), filled(1));
    ASSERT_EQ(fills_.size(), 1U);
    EXPECT_EQ(fills_[0].makerOrderId, 2U);
    EXPECT_EQ(fills_[0].price, 100);
}

TEST_F(OrderBookTest, MarketOrderSweepsLevelsAndCancelsTheRest) {
    book_.submit(limit(1, kBob, Side::Sell, 100, 2), fills_);
    book_.submit(limit(2, kBob, Side::Sell, 101, 2), fills_);

    EXPECT_EQ(book_.submit(market(3, kAlice, Side::Buy, 5), fills_),
              cancelled(CancelReason::ImmediateOrCancel, 4));
    EXPECT_EQ(fills_.size(), 2U);
    EXPECT_EQ(book_.bestAsk(), std::nullopt);
    EXPECT_EQ(book_.orderCount(), 0U);
}

TEST_F(OrderBookTest, MarketOrderIntoAnEmptySideIsCancelled) {
    EXPECT_EQ(book_.submit(market(1, kAlice, Side::Sell, 5), fills_),
              cancelled(CancelReason::ImmediateOrCancel));
    EXPECT_TRUE(fills_.empty());
}

TEST_F(OrderBookTest, ImmediateOrCancelNeverRests) {
    book_.submit(limit(1, kBob, Side::Sell, 100, 2), fills_);

    EXPECT_EQ(book_.submit(limit(2, kAlice, Side::Buy, 100, 5, TimeInForce::ImmediateOrCancel),
                           fills_),
              cancelled(CancelReason::ImmediateOrCancel, 2));
    EXPECT_EQ(book_.submit(limit(3, kAlice, Side::Buy, 99, 5, TimeInForce::ImmediateOrCancel),
                           fills_),
              cancelled(CancelReason::ImmediateOrCancel));
    EXPECT_EQ(book_.bestBid(), std::nullopt);
    EXPECT_EQ(book_.orderCount(), 0U);
}

TEST_F(OrderBookTest, SelfTradeCancelsTheIncomingOrder) {
    book_.submit(limit(1, kCarol, Side::Sell, 100, 2), fills_);
    book_.submit(limit(2, kAlice, Side::Sell, 100, 2), fills_);
    book_.submit(limit(3, kBob, Side::Sell, 100, 2), fills_);

    // Alice's buy trades with Carol, who is ahead in the queue, then reaches her own order.
    EXPECT_EQ(book_.submit(limit(4, kAlice, Side::Buy, 101, 5), fills_),
              cancelled(CancelReason::SelfTrade, 2));
    ASSERT_EQ(fills_.size(), 1U);
    EXPECT_EQ(fills_[0].makerOrderId, 1U);
    EXPECT_EQ(book_.depth(Side::Sell),
              (std::vector<LevelSummary>{{.price = 100, .quantity = 4, .orderCount = 2}}));
    EXPECT_EQ(book_.bestBid(), std::nullopt);
}

TEST_F(OrderBookTest, CancelRemovesTheOrderAndItsEmptyLevel) {
    book_.submit(limit(1, kAlice, Side::Buy, 99, 5), fills_);
    book_.submit(limit(2, kBob, Side::Buy, 99, 1), fills_);

    EXPECT_EQ(book_.cancel(1), (RestingOrder{.id = 1,
                                             .owner = kAlice,
                                             .side = Side::Buy,
                                             .price = 99,
                                             .remaining = 5}));
    EXPECT_EQ(book_.depth(Side::Buy),
              (std::vector<LevelSummary>{{.price = 99, .quantity = 1, .orderCount = 1}}));
    EXPECT_EQ(book_.cancel(1), std::nullopt);

    book_.cancel(2);
    EXPECT_EQ(book_.bestBid(), std::nullopt);
    EXPECT_TRUE(book_.depth(Side::Buy).empty());
}

TEST_F(OrderBookTest, ReducingQuantityKeepsQueuePosition) {
    book_.submit(limit(1, kAlice, Side::Sell, 100, 5), fills_);
    book_.submit(limit(2, kBob, Side::Sell, 100, 5), fills_);

    EXPECT_EQ(book_.modify(1, 100, 2, fills_), resting(2));
    book_.submit(market(3, kCarol, Side::Buy, 3), fills_);
    ASSERT_EQ(fills_.size(), 2U);
    EXPECT_EQ(fills_[0].makerOrderId, 1U);
    EXPECT_EQ(fills_[0].quantity, 2);
    EXPECT_EQ(fills_[1].makerOrderId, 2U);
}

TEST_F(OrderBookTest, IncreasingQuantityLosesQueuePosition) {
    book_.submit(limit(1, kAlice, Side::Sell, 100, 2), fills_);
    book_.submit(limit(2, kBob, Side::Sell, 100, 2), fills_);

    EXPECT_EQ(book_.modify(1, 100, 4, fills_), resting(4));
    book_.submit(market(3, kCarol, Side::Buy, 3), fills_);
    ASSERT_EQ(fills_.size(), 2U);
    EXPECT_EQ(fills_[0].makerOrderId, 2U);
    EXPECT_EQ(fills_[1].makerOrderId, 1U);
}

TEST_F(OrderBookTest, RepricedOrderCanTradeImmediately) {
    book_.submit(limit(1, kAlice, Side::Buy, 99, 2), fills_);
    book_.submit(limit(2, kBob, Side::Sell, 100, 3), fills_);

    EXPECT_EQ(book_.modify(1, 100, 2, fills_), filled(2));
    ASSERT_EQ(fills_.size(), 1U);
    EXPECT_EQ(fills_[0].takerOrderId, 1U);
    EXPECT_EQ(book_.find(1), std::nullopt);
    EXPECT_EQ(book_.find(2), (RestingOrder{.id = 2,
                                           .owner = kBob,
                                           .side = Side::Sell,
                                           .price = 100,
                                           .remaining = 1}));
}

TEST_F(OrderBookTest, RejectsInvalidRequestsWithoutChangingTheBook) {
    book_.submit(limit(1, kAlice, Side::Buy, 99, 5), fills_);

    EXPECT_EQ(book_.submit(limit(2, kAlice, Side::Buy, 99, 0), fills_),
              rejected(RejectReason::NonPositiveQuantity));
    EXPECT_EQ(book_.submit(limit(3, kAlice, Side::Buy, 99, -1), fills_),
              rejected(RejectReason::NonPositiveQuantity));
    EXPECT_EQ(book_.submit(limit(1, kBob, Side::Sell, 98, 1), fills_),
              rejected(RejectReason::DuplicateOrderId));
    EXPECT_EQ(book_.modify(7, 99, 1, fills_), rejected(RejectReason::UnknownOrderId));
    EXPECT_EQ(book_.modify(1, 99, 0, fills_), rejected(RejectReason::NonPositiveQuantity));

    EXPECT_TRUE(fills_.empty());
    EXPECT_EQ(book_.depth(Side::Buy),
              (std::vector<LevelSummary>{{.price = 99, .quantity = 5, .orderCount = 1}}));
    EXPECT_TRUE(book_.depth(Side::Sell).empty());
}

TEST_F(OrderBookTest, DepthListsLevelsBestFirst) {
    book_.submit(limit(1, kAlice, Side::Buy, 98, 5), fills_);
    book_.submit(limit(2, kAlice, Side::Buy, 99, 1), fills_);
    book_.submit(limit(3, kBob, Side::Buy, 99, 2), fills_);
    book_.submit(limit(4, kCarol, Side::Sell, 103, 1), fills_);
    book_.submit(limit(5, kCarol, Side::Sell, 101, 4), fills_);

    EXPECT_EQ(book_.depth(Side::Buy),
              (std::vector<LevelSummary>{{.price = 99, .quantity = 3, .orderCount = 2},
                                         {.price = 98, .quantity = 5, .orderCount = 1}}));
    EXPECT_EQ(book_.depth(Side::Buy, 1),
              (std::vector<LevelSummary>{{.price = 99, .quantity = 3, .orderCount = 2}}));
    EXPECT_EQ(book_.depth(Side::Sell),
              (std::vector<LevelSummary>{{.price = 101, .quantity = 4, .orderCount = 1},
                                         {.price = 103, .quantity = 1, .orderCount = 1}}));
}

TEST_F(OrderBookTest, BestLevelSummarizesTheTopOfEachSide) {
    EXPECT_EQ(book_.bestLevel(Side::Buy), std::nullopt);

    book_.submit(limit(1, kAlice, Side::Buy, 98, 5), fills_);
    book_.submit(limit(2, kAlice, Side::Buy, 99, 1), fills_);
    book_.submit(limit(3, kBob, Side::Buy, 99, 2), fills_);

    EXPECT_EQ(book_.bestLevel(Side::Buy),
              (LevelSummary{.price = 99, .quantity = 3, .orderCount = 2}));
    EXPECT_EQ(book_.bestLevel(Side::Sell), std::nullopt);
}

} // namespace
} // namespace crowdbook

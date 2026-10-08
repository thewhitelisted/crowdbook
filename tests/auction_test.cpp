#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/order_book.hpp"

namespace crowdbook {
namespace {

// A book in a call auction, with orders added by owner, side, price and size.
class CallBook {
public:
    CallBook() { book.setMatching(false); }

    OrderId add(AgentId owner, Side side, Price price, Quantity quantity) {
        const OrderId id = nextId_++;
        std::vector<Fill> fills;
        const OrderResult result = book.submit(
            {.id = id, .owner = owner, .side = side, .price = price, .quantity = quantity},
            fills);
        EXPECT_EQ(result.status, OrderStatus::Resting);
        EXPECT_TRUE(fills.empty());
        return id;
    }

    OrderBook book;

private:
    OrderId nextId_ = 1;
};

TEST(AuctionTest, LimitOrdersRestCrossedAndMarketOrdersCannotWait) {
    CallBook call;
    call.add(1, Side::Buy, 105, 5);
    call.add(2, Side::Sell, 100, 5);
    EXPECT_EQ(call.book.bestBid(), 105);
    EXPECT_EQ(call.book.bestAsk(), 100);
    EXPECT_EQ(call.book.audit(), std::nullopt); // crossed is fine in a call
    std::vector<Fill> fills;
    const OrderResult market = call.book.submit(
        {.id = 9, .owner = 3, .side = Side::Buy, .type = OrderType::Market, .quantity = 2},
        fills);
    EXPECT_EQ(market.status, OrderStatus::Cancelled);
    EXPECT_EQ(market.filled, 0);
    EXPECT_TRUE(fills.empty());
}

TEST(AuctionTest, TheUncrossTradesTheMostLots) {
    CallBook call;
    call.add(1, Side::Buy, 102, 3);
    call.add(2, Side::Buy, 101, 5);
    call.add(3, Side::Sell, 100, 2);
    call.add(4, Side::Sell, 101, 10);
    // At 100: 8 bid, 2 offered. At 101: 8 bid, 12 offered. At 102: 3 bid, 12 offered.
    EXPECT_EQ(call.book.indicative(100),
              (Uncross{.price = 101, .volume = 8, .imbalance = -4}));
}

TEST(AuctionTest, EqualVolumesGoToTheSmallestImbalance) {
    CallBook call;
    call.add(1, Side::Buy, 101, 5);
    call.add(2, Side::Sell, 99, 5);
    call.add(3, Side::Sell, 100, 2);
    // Five lots trade at 99, 100 and 101, but only 99 leaves nothing over.
    EXPECT_EQ(call.book.indicative(101), (Uncross{.price = 99, .volume = 5, .imbalance = 0}));
}

TEST(AuctionTest, ThenTheReferenceThenTheLowestPrice) {
    CallBook call;
    call.add(1, Side::Buy, 102, 5);
    call.add(2, Side::Sell, 100, 5);
    EXPECT_EQ(call.book.indicative(102)->price, 102);
    EXPECT_EQ(call.book.indicative(100)->price, 100);
    EXPECT_EQ(call.book.indicative(101)->price, 100); // as near as 102, and lower
}

TEST(AuctionTest, NothingCrossesNothingTrades) {
    CallBook call;
    call.add(1, Side::Buy, 99, 5);
    call.add(2, Side::Sell, 100, 5);
    EXPECT_EQ(call.book.indicative(100), std::nullopt);
    std::vector<Fill> fills;
    std::vector<RestingOrder> selfTrades;
    EXPECT_EQ(call.book.uncross(100, fills, selfTrades), std::nullopt);
    EXPECT_EQ(call.book.orderCount(), 2U);
}

TEST(AuctionTest, EveryoneTradesAtOnePriceOldestFirst) {
    CallBook call;
    const OrderId older = call.add(1, Side::Buy, 101, 4);
    const OrderId newer = call.add(2, Side::Buy, 101, 4);
    const OrderId seller = call.add(3, Side::Sell, 99, 6);
    std::vector<Fill> fills;
    std::vector<RestingOrder> selfTrades;
    const std::optional<Uncross> uncross = call.book.uncross(100, fills, selfTrades);
    ASSERT_TRUE(uncross);
    // Only the orders' prices are candidates: 6 lots trade at 99 and at 101, each leaving 2 over
    // and each a tick from the reference, so the lower wins.
    EXPECT_EQ(uncross->price, 99);
    ASSERT_EQ(fills.size(), 2U);
    // The older bid trades first, and fully; each fill's maker is the older order.
    EXPECT_EQ(fills[0], (Fill{.makerOrderId = older,
                              .takerOrderId = seller,
                              .makerOwner = 1,
                              .takerOwner = 3,
                              .takerSide = Side::Sell,
                              .price = 99,
                              .quantity = 4,
                              .makerRemaining = 0}));
    EXPECT_EQ(fills[1].makerOrderId, newer);
    EXPECT_EQ(fills[1].takerOrderId, seller);
    EXPECT_EQ(fills[1].quantity, 2);
    EXPECT_EQ(fills[1].makerRemaining, 2);
    EXPECT_EQ(call.book.find(newer)->remaining, 2);
    call.book.setMatching(true);
    EXPECT_EQ(call.book.audit(), std::nullopt);
}

TEST(AuctionTest, AnOwnerNeverTradesWithItselfInAnUncross) {
    CallBook call;
    call.add(1, Side::Buy, 102, 5);
    const OrderId own = call.add(1, Side::Sell, 100, 5);
    call.add(2, Side::Sell, 101, 5);
    std::vector<Fill> fills;
    std::vector<RestingOrder> selfTrades;
    // Five lots trade at 100 with nothing over, but the only offer at 100 is owner 1's own: the
    // newer of the pair, the offer, is cancelled, and nothing trades at 100.
    const std::optional<Uncross> first = call.book.uncross(101, fills, selfTrades);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->price, 100);
    ASSERT_EQ(selfTrades.size(), 1U);
    EXPECT_EQ(selfTrades[0].id, own);
    EXPECT_TRUE(fills.empty());
    // That leaves the book crossed, so it uncrosses again, at the price for what is left.
    const std::optional<Uncross> second = call.book.uncross(101, fills, selfTrades);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->price, 101);
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_EQ(fills[0].makerOwner, 1U);
    EXPECT_EQ(fills[0].takerOwner, 2U);
    EXPECT_EQ(fills[0].price, 101);
    EXPECT_EQ(selfTrades.size(), 1U);
    call.book.setMatching(true);
    EXPECT_EQ(call.book.audit(), std::nullopt);
}

} // namespace
} // namespace crowdbook

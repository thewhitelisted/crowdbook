#include <stdexcept>

#include <gtest/gtest.h>

#include "crowdbook/ledger.hpp"
#include "exchange_test_support.hpp"

namespace crowdbook {
namespace {

using test::limitOrder;
using test::marketOrder;

constexpr AgentId kAlice = 1;

OrderAccepted accepted(ClientOrderId clientOrderId, OrderId orderId) {
    return {.agent = kAlice, .clientOrderId = clientOrderId, .orderId = orderId};
}

OrderFilled filled(ClientOrderId clientOrderId, Side side, Price price, Quantity quantity,
                   Quantity leaves) {
    return {.agent = kAlice,
            .clientOrderId = clientOrderId,
            .side = side,
            .price = price,
            .quantity = quantity,
            .leavesQuantity = leaves};
}

OrderRejected rejected(ClientOrderId clientOrderId, RequestKind request) {
    return {.agent = kAlice,
            .clientOrderId = clientOrderId,
            .request = request,
            .reason = RejectReason::UnknownOrderId};
}

TEST(LedgerTest, StartsFromTheGivenBalances) {
    const Ledger ledger{1'000, -5};
    EXPECT_EQ(ledger.cash(), 1'000);
    EXPECT_EQ(ledger.position(), -5);
    EXPECT_TRUE(ledger.orders().empty());
}

TEST(LedgerTest, FollowsAnOrderFromRequestToFullFill) {
    Ledger ledger;
    ledger.recordRequest(limitOrder(1, Side::Buy, 99, 5));
    EXPECT_EQ(*ledger.find(1),
              (OwnOrder{.clientOrderId = 1, .side = Side::Buy, .price = 99, .leaves = 5}));

    ledger.apply(accepted(1, 17));
    EXPECT_TRUE(ledger.find(1)->acknowledged);
    EXPECT_EQ(ledger.find(1)->orderId, 17U);

    ledger.apply(filled(1, Side::Buy, 99, 2, 3));
    EXPECT_EQ(ledger.position(), 2);
    EXPECT_EQ(ledger.cash(), -198);
    EXPECT_EQ(ledger.find(1)->leaves, 3);

    ledger.apply(filled(1, Side::Buy, 99, 3, 0));
    EXPECT_EQ(ledger.find(1), nullptr);
    EXPECT_EQ(ledger.position(), 5);
    EXPECT_EQ(ledger.cash(), -495);
}

TEST(LedgerTest, AddsUpFeesAndRebatesFromFills) {
    Ledger ledger;
    ledger.recordRequest(limitOrder(1, Side::Buy, 99, 5));
    ledger.apply(accepted(1, 17));
    OrderFilled maker = filled(1, Side::Buy, 99, 2, 3);
    maker.fee = -400;
    OrderFilled taker = filled(1, Side::Buy, 99, 3, 0);
    taker.fee = 900;
    ledger.apply(maker);
    ledger.apply(taker);
    EXPECT_EQ(ledger.fees(), 500);
    EXPECT_EQ(ledger.cash(), -495); // fees never touch cash
}

TEST(LedgerTest, SellingWithoutSharesGoesShort) {
    Ledger ledger;
    ledger.recordRequest(marketOrder(1, Side::Sell, 3));
    ledger.apply(accepted(1, 1));
    ledger.apply(filled(1, Side::Sell, 100, 3, 0));
    EXPECT_EQ(ledger.position(), -3);
    EXPECT_EQ(ledger.cash(), 300);
}

TEST(LedgerTest, ForgetsARejectedOrder) {
    Ledger ledger;
    ledger.recordRequest(limitOrder(1, Side::Buy, 99, 5));
    ledger.apply(rejected(1, RequestKind::New));
    EXPECT_TRUE(ledger.orders().empty());
}

TEST(LedgerTest, FailedCancelLeavesTheOrderOpen) {
    Ledger ledger;
    ledger.recordRequest(limitOrder(1, Side::Buy, 99, 5));
    ledger.apply(accepted(1, 1));
    ledger.recordRequest(CancelOrder{.clientOrderId = 1});
    EXPECT_TRUE(ledger.find(1)->cancelRequested);

    ledger.apply(rejected(1, RequestKind::Cancel));
    EXPECT_FALSE(ledger.find(1)->cancelRequested);
}

TEST(LedgerTest, IgnoresALateRejectionForAnOrderThatAlreadyFinished) {
    Ledger ledger;
    ledger.recordRequest(limitOrder(1, Side::Buy, 99, 5));
    ledger.apply(accepted(1, 1));
    ledger.recordRequest(CancelOrder{.clientOrderId = 1});
    ledger.apply(filled(1, Side::Buy, 99, 5, 0)); // filled before the cancel arrived

    EXPECT_NO_THROW(ledger.apply(rejected(1, RequestKind::Cancel)));
    EXPECT_NO_THROW(ledger.apply(rejected(1, RequestKind::Modify)));
    EXPECT_TRUE(ledger.orders().empty());
    EXPECT_EQ(ledger.position(), 5);
}

TEST(LedgerTest, ModifiedAndCancelledUpdateTheOrder) {
    Ledger ledger;
    ledger.recordRequest(limitOrder(1, Side::Sell, 101, 5));
    ledger.apply(accepted(1, 1));
    ledger.recordRequest(ModifyOrder{.clientOrderId = 1, .price = 102, .quantity = 2});
    EXPECT_EQ(ledger.find(1)->price, 101); // unchanged until confirmed

    ledger.apply(OrderModified{
        .agent = kAlice, .clientOrderId = 1, .orderId = 1, .price = 102, .quantity = 2});
    EXPECT_EQ(ledger.find(1)->price, 102);
    EXPECT_EQ(ledger.find(1)->leaves, 2);

    ledger.apply(OrderCancelled{.agent = kAlice, .clientOrderId = 1, .orderId = 1, .quantity = 2});
    EXPECT_TRUE(ledger.orders().empty());
}

TEST(LedgerTest, OpenQuantityCountsOrdersInFlight) {
    Ledger ledger;
    ledger.recordRequest(limitOrder(1, Side::Buy, 99, 5));
    ledger.apply(accepted(1, 1));
    ledger.recordRequest(limitOrder(2, Side::Buy, 98, 3)); // not yet acknowledged
    ledger.recordRequest(limitOrder(3, Side::Sell, 101, 4));
    EXPECT_EQ(ledger.openQuantity(Side::Buy), 8);
    EXPECT_EQ(ledger.openQuantity(Side::Sell), 4);
}

TEST(LedgerTest, RejectsEventsForOrdersItNeverSent) {
    Ledger ledger;
    ledger.recordRequest(limitOrder(1, Side::Buy, 99, 5));
    EXPECT_THROW(ledger.recordRequest(limitOrder(1, Side::Buy, 98, 1)), std::logic_error);
    EXPECT_THROW(ledger.apply(accepted(2, 2)), std::logic_error);
    EXPECT_THROW(ledger.apply(filled(2, Side::Buy, 99, 1, 0)), std::logic_error);
    EXPECT_THROW(ledger.apply(rejected(2, RequestKind::New)), std::logic_error);
    EXPECT_EQ(ledger.position(), 0); // a rejected fill must not touch the balances
}

TEST(LedgerTest, IgnoresPublicMarketData) {
    Ledger ledger;
    ledger.apply(Trade{.price = 100, .quantity = 5});
    ledger.apply(TopOfBook{});
    EXPECT_EQ(ledger.position(), 0);
    EXPECT_TRUE(ledger.orders().empty());
}

} // namespace
} // namespace crowdbook

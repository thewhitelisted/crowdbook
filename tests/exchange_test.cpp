#include <optional>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/exchange.hpp"
#include "exchange_test_support.hpp"

namespace crowdbook {
namespace {

using test::limitOrder;
using test::marketOrder;

constexpr AgentId kAlice = 1;
constexpr AgentId kBob = 2;
constexpr AgentId kDave = 4; // registered with tight limits by the tests that need them

// Alice and Bob have default (effectively unlimited) accounts. Every test ends with an audit.
class ExchangeTest : public ::testing::Test {
protected:
    ExchangeTest() {
        exchange_.addAgent(kAlice);
        exchange_.addAgent(kBob);
    }

    void TearDown() override { EXPECT_EQ(exchange_.audit(), std::nullopt); }

    // Sends one request and returns just the events it produced.
    std::vector<Event> send(AgentId agent, const Request& request) {
        std::vector<Event> events;
        exchange_.handle(agent, request, events);
        return events;
    }

    Exchange exchange_;
};

TEST_F(ExchangeTest, AcceptsAndRestsALimitOrder) {
    EXPECT_EQ(send(kAlice, limitOrder(7, Side::Buy, 99, 5)),
              (std::vector<Event>{
                  OrderAccepted{.agent = kAlice,
                                .clientOrderId = 7,
                                .orderId = 1,
                                .side = Side::Buy,
                                .type = OrderType::Limit,
                                .timeInForce = TimeInForce::GoodTillCancel,
                                .price = 99,
                                .quantity = 5},
                  TopOfBook{.bid = LevelSummary{.price = 99, .quantity = 5, .orderCount = 1}},
              }));
    EXPECT_EQ(exchange_.liveOrderId(kAlice, 7), OrderId{1});
    EXPECT_EQ(exchange_.account(kAlice), (Account{.openBuyQuantity = 5}));
}

TEST_F(ExchangeTest, SettlesBothSidesOfATrade) {
    send(kBob, limitOrder(1, Side::Sell, 101, 3));

    EXPECT_EQ(send(kAlice, limitOrder(1, Side::Buy, 101, 5)),
              (std::vector<Event>{
                  OrderAccepted{.agent = kAlice,
                                .clientOrderId = 1,
                                .orderId = 2,
                                .side = Side::Buy,
                                .price = 101,
                                .quantity = 5},
                  OrderFilled{.agent = kBob,
                              .clientOrderId = 1,
                              .orderId = 1,
                              .side = Side::Sell,
                              .price = 101,
                              .quantity = 3,
                              .leavesQuantity = 0,
                              .liquidity = Liquidity::Maker},
                  OrderFilled{.agent = kAlice,
                              .clientOrderId = 1,
                              .orderId = 2,
                              .side = Side::Buy,
                              .price = 101,
                              .quantity = 3,
                              .leavesQuantity = 2,
                              .liquidity = Liquidity::Taker},
                  Trade{.price = 101, .quantity = 3, .aggressorSide = Side::Buy},
                  TopOfBook{.bid = LevelSummary{.price = 101, .quantity = 2, .orderCount = 1}},
              }));
    EXPECT_EQ(exchange_.account(kAlice),
              (Account{.cash = -303, .position = 3, .openBuyQuantity = 2}));
    // Bob sold shares he did not have: short selling is allowed within the position limit.
    EXPECT_EQ(exchange_.account(kBob), (Account{.cash = 303, .position = -3}));
}

TEST_F(ExchangeTest, RejectsInvalidRequestsWithASingleEvent) {
    exchange_.addAgent(kDave, {.maxOrderQuantity = 10});
    send(kAlice, limitOrder(1, Side::Buy, 99, 5));

    const auto rejection = [](AgentId agent, ClientOrderId clientOrderId, RequestKind request,
                              RejectReason reason) {
        return std::vector<Event>{OrderRejected{
            .agent = agent, .clientOrderId = clientOrderId, .request = request, .reason = reason}};
    };
    EXPECT_EQ(send(99, limitOrder(1, Side::Buy, 99, 5)),
              rejection(99, 1, RequestKind::New, RejectReason::UnknownAgent));
    EXPECT_EQ(send(kAlice, limitOrder(2, Side::Buy, 99, 0)),
              rejection(kAlice, 2, RequestKind::New, RejectReason::NonPositiveQuantity));
    EXPECT_EQ(send(kDave, limitOrder(2, Side::Buy, 99, 11)),
              rejection(kDave, 2, RequestKind::New, RejectReason::OrderSizeLimit));
    EXPECT_EQ(send(kAlice, limitOrder(2, Side::Buy, 0, 5)),
              rejection(kAlice, 2, RequestKind::New, RejectReason::InvalidPrice));
    EXPECT_EQ(send(kAlice, limitOrder(2, Side::Buy, kMaxPrice + 1, 5)),
              rejection(kAlice, 2, RequestKind::New, RejectReason::InvalidPrice));
    EXPECT_EQ(send(kAlice, limitOrder(1, Side::Sell, 101, 5)),
              rejection(kAlice, 1, RequestKind::New, RejectReason::DuplicateClientOrderId));
    EXPECT_EQ(send(kAlice, CancelOrder{.clientOrderId = 9}),
              rejection(kAlice, 9, RequestKind::Cancel, RejectReason::UnknownOrderId));
    EXPECT_EQ(send(kAlice, ModifyOrder{.clientOrderId = 9, .price = 99, .quantity = 1}),
              rejection(kAlice, 9, RequestKind::Modify, RejectReason::UnknownOrderId));
    EXPECT_EQ(send(kAlice, ModifyOrder{.clientOrderId = 1, .price = 99, .quantity = 0}),
              rejection(kAlice, 1, RequestKind::Modify, RejectReason::NonPositiveQuantity));
    EXPECT_EQ(send(kAlice, ModifyOrder{.clientOrderId = 1, .price = 0, .quantity = 5}),
              rejection(kAlice, 1, RequestKind::Modify, RejectReason::InvalidPrice));

    EXPECT_EQ(exchange_.account(kAlice), (Account{.openBuyQuantity = 5}));
    EXPECT_EQ(exchange_.book().orderCount(), 1U);
}

TEST_F(ExchangeTest, PositionLimitCountsOpenOrdersAsFilled) {
    exchange_.addAgent(kDave, {.maxPosition = 10});
    const auto rejectedFor = [](ClientOrderId clientOrderId) {
        return std::vector<Event>{OrderRejected{.agent = kDave,
                                                .clientOrderId = clientOrderId,
                                                .request = RequestKind::New,
                                                .reason = RejectReason::PositionLimit}};
    };

    send(kDave, limitOrder(1, Side::Buy, 99, 6));
    EXPECT_EQ(send(kDave, limitOrder(2, Side::Buy, 98, 5)), rejectedFor(2)); // 6 + 5 > 10
    send(kDave, limitOrder(3, Side::Buy, 98, 4));                             // 6 + 4 = 10
    EXPECT_EQ(exchange_.account(kDave).openBuyQuantity, 10);

    // The short side has its own headroom: open buys do not offset it.
    send(kDave, limitOrder(4, Side::Sell, 101, 10));
    EXPECT_EQ(send(kDave, limitOrder(5, Side::Sell, 102, 1)), rejectedFor(5));
    EXPECT_EQ(exchange_.account(kDave),
              (Account{.openBuyQuantity = 10, .openSellQuantity = 10}));
}

TEST_F(ExchangeTest, ShortPositionCanBeBoughtBack) {
    exchange_.addAgent(kDave, {.maxPosition = 5});
    send(kAlice, limitOrder(1, Side::Buy, 100, 5));
    send(kDave, marketOrder(1, Side::Sell, 5));
    EXPECT_EQ(exchange_.account(kDave).position, -5);

    EXPECT_EQ(send(kDave, marketOrder(2, Side::Sell, 1)).front(),
              (Event{OrderRejected{.agent = kDave,
                                   .clientOrderId = 2,
                                   .request = RequestKind::New,
                                   .reason = RejectReason::PositionLimit}}));
    // Buying 10 would leave Dave at +5 even if it all filled, which is within the limit.
    EXPECT_EQ(send(kDave, limitOrder(3, Side::Buy, 99, 10)).front(),
              (Event{OrderAccepted{.agent = kDave,
                                   .clientOrderId = 3,
                                   .orderId = 3,
                                   .side = Side::Buy,
                                   .price = 99,
                                   .quantity = 10}}));
}

TEST_F(ExchangeTest, ModifyChecksRiskOnlyWhenQuantityGrows) {
    exchange_.addAgent(kDave, {.maxPosition = 10});
    send(kDave, limitOrder(1, Side::Buy, 99, 8));

    EXPECT_EQ(send(kDave, ModifyOrder{.clientOrderId = 1, .price = 99, .quantity = 11}),
              (std::vector<Event>{OrderRejected{.agent = kDave,
                                                .clientOrderId = 1,
                                                .request = RequestKind::Modify,
                                                .reason = RejectReason::PositionLimit}}));
    EXPECT_EQ(send(kDave, ModifyOrder{.clientOrderId = 1, .price = 99, .quantity = 10}).front(),
              (Event{OrderModified{
                  .agent = kDave, .clientOrderId = 1, .orderId = 1, .price = 99, .quantity = 10}}));
    EXPECT_EQ(send(kDave, ModifyOrder{.clientOrderId = 1, .price = 97, .quantity = 2}).front(),
              (Event{OrderModified{
                  .agent = kDave, .clientOrderId = 1, .orderId = 1, .price = 97, .quantity = 2}}));
    EXPECT_EQ(exchange_.account(kDave).openBuyQuantity, 2);
}

TEST_F(ExchangeTest, CancelReleasesOpenQuantity) {
    send(kAlice, limitOrder(1, Side::Buy, 99, 5));

    EXPECT_EQ(send(kAlice, CancelOrder{.clientOrderId = 1}),
              (std::vector<Event>{OrderCancelled{.agent = kAlice,
                                                 .clientOrderId = 1,
                                                 .orderId = 1,
                                                 .quantity = 5,
                                                 .reason = CancelReason::Requested},
                                  TopOfBook{}}));
    EXPECT_EQ(exchange_.account(kAlice), Account{});
    EXPECT_EQ(exchange_.liveOrderId(kAlice, 1), std::nullopt);
}

TEST_F(ExchangeTest, AgentsCannotNameEachOthersOrders) {
    send(kAlice, limitOrder(1, Side::Buy, 99, 5));

    EXPECT_EQ(send(kBob, CancelOrder{.clientOrderId = 1}),
              (std::vector<Event>{OrderRejected{.agent = kBob,
                                                .clientOrderId = 1,
                                                .request = RequestKind::Cancel,
                                                .reason = RejectReason::UnknownOrderId}}));
    EXPECT_EQ(exchange_.liveOrderId(kAlice, 1), OrderId{1});
}

TEST_F(ExchangeTest, ReportsTheCancelledRemainderOfAnImmediateOrCancelOrder) {
    send(kBob, limitOrder(1, Side::Sell, 100, 2));

    const std::vector<Event> events =
        send(kAlice, limitOrder(1, Side::Buy, 100, 5, TimeInForce::ImmediateOrCancel));
    ASSERT_EQ(events.size(), 6U); // accepted, two fills, trade, cancel, top of book
    EXPECT_EQ(events[4], (Event{OrderCancelled{.agent = kAlice,
                                               .clientOrderId = 1,
                                               .orderId = 2,
                                               .quantity = 3,
                                               .reason = CancelReason::ImmediateOrCancel}}));
    EXPECT_EQ(events[5], (Event{TopOfBook{}}));
    EXPECT_EQ(exchange_.account(kAlice), (Account{.cash = -200, .position = 2}));
}

TEST_F(ExchangeTest, MarketOrderTakesWhatItCanAndCancelsTheRest) {
    send(kBob, limitOrder(1, Side::Sell, 100, 2));
    send(kBob, limitOrder(2, Side::Sell, 101, 2));

    const std::vector<Event> events = send(kAlice, marketOrder(1, Side::Buy, 5));
    EXPECT_EQ(events.at(events.size() - 2),
              (Event{OrderCancelled{.agent = kAlice,
                                    .clientOrderId = 1,
                                    .orderId = 3,
                                    .quantity = 1,
                                    .reason = CancelReason::ImmediateOrCancel}}));
    EXPECT_EQ(exchange_.account(kAlice), (Account{.cash = -402, .position = 4}));
    EXPECT_EQ(exchange_.account(kBob), (Account{.cash = 402, .position = -4}));
}

TEST_F(ExchangeTest, ReportsSelfTradePrevention) {
    send(kAlice, limitOrder(1, Side::Sell, 100, 2));

    EXPECT_EQ(send(kAlice, limitOrder(2, Side::Buy, 100, 3)),
              (std::vector<Event>{OrderAccepted{.agent = kAlice,
                                                .clientOrderId = 2,
                                                .orderId = 2,
                                                .side = Side::Buy,
                                                .price = 100,
                                                .quantity = 3},
                                  OrderCancelled{.agent = kAlice,
                                                 .clientOrderId = 2,
                                                 .orderId = 2,
                                                 .quantity = 3,
                                                 .reason = CancelReason::SelfTrade}}));
}

TEST_F(ExchangeTest, ModifyThatCrossesTradesImmediately) {
    send(kAlice, limitOrder(1, Side::Buy, 99, 2));
    send(kBob, limitOrder(1, Side::Sell, 100, 3));

    EXPECT_EQ(send(kAlice, ModifyOrder{.clientOrderId = 1, .price = 100, .quantity = 2}),
              (std::vector<Event>{
                  OrderModified{.agent = kAlice,
                                .clientOrderId = 1,
                                .orderId = 1,
                                .price = 100,
                                .quantity = 2},
                  OrderFilled{.agent = kBob,
                              .clientOrderId = 1,
                              .orderId = 2,
                              .side = Side::Sell,
                              .price = 100,
                              .quantity = 2,
                              .leavesQuantity = 1,
                              .liquidity = Liquidity::Maker},
                  OrderFilled{.agent = kAlice,
                              .clientOrderId = 1,
                              .orderId = 1,
                              .side = Side::Buy,
                              .price = 100,
                              .quantity = 2,
                              .leavesQuantity = 0,
                              .liquidity = Liquidity::Taker},
                  Trade{.price = 100, .quantity = 2, .aggressorSide = Side::Buy},
                  TopOfBook{.ask = LevelSummary{.price = 100, .quantity = 1, .orderCount = 1}},
              }));
    EXPECT_EQ(exchange_.account(kAlice), (Account{.cash = -200, .position = 2}));
    EXPECT_EQ(exchange_.account(kBob),
              (Account{.cash = 200, .position = -2, .openSellQuantity = 1}));
}

TEST_F(ExchangeTest, PublishesTopOfBookOnlyWhenItChanges) {
    EXPECT_EQ(send(kAlice, limitOrder(1, Side::Buy, 99, 5)).size(), 2U);
    EXPECT_EQ(send(kBob, limitOrder(1, Side::Buy, 98, 1)).size(), 1U); // behind the best bid
    EXPECT_EQ(send(kBob, limitOrder(2, Side::Buy, 99, 1)).back(),
              (Event{TopOfBook{.bid = LevelSummary{.price = 99, .quantity = 6, .orderCount = 2}}}));
}

TEST_F(ExchangeTest, ClientOrderIdIsFreeAgainOnceTheOrderIsDone) {
    send(kAlice, limitOrder(1, Side::Buy, 99, 5));
    send(kAlice, CancelOrder{.clientOrderId = 1});

    EXPECT_EQ(send(kAlice, limitOrder(1, Side::Buy, 98, 2)).front(),
              (Event{OrderAccepted{.agent = kAlice,
                                   .clientOrderId = 1,
                                   .orderId = 2,
                                   .side = Side::Buy,
                                   .price = 98,
                                   .quantity = 2}}));
}

TEST(ExchangeSetupTest, RejectsDuplicateAgentsAndInvalidLimits) {
    Exchange exchange;
    exchange.addAgent(kAlice);
    EXPECT_THROW(exchange.addAgent(kAlice), std::invalid_argument);
    EXPECT_THROW(exchange.addAgent(kBob, {.maxPosition = -1}), std::invalid_argument);
    EXPECT_THROW(exchange.addAgent(kBob, {.maxOrderQuantity = 0}), std::invalid_argument);
    EXPECT_THROW((exchange.addAgent(kBob, {.initialPosition = 11, .maxPosition = 10})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(exchange.account(kBob)), std::out_of_range);
}

TEST(AccountTest, EquityMarksThePositionToMarket) {
    EXPECT_EQ((Account{.cash = -303, .position = 3}).equity(105), 12);
    EXPECT_EQ((Account{.cash = 303, .position = -3}).equity(105), -12);
}

} // namespace
} // namespace crowdbook

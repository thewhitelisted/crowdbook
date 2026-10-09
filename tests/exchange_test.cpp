#include <algorithm>
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

TEST_F(ExchangeTest, PostOnlyOrderRestsWhenItWouldNotTrade) {
    send(kBob, limitOrder(1, Side::Sell, 101, 3));

    EXPECT_EQ(send(kAlice, limitOrder(1, Side::Buy, 100, 5, TimeInForce::PostOnly)),
              (std::vector<Event>{
                  OrderAccepted{.agent = kAlice,
                                .clientOrderId = 1,
                                .orderId = 2,
                                .side = Side::Buy,
                                .timeInForce = TimeInForce::PostOnly,
                                .price = 100,
                                .quantity = 5},
                  TopOfBook{.bid = LevelSummary{.price = 100, .quantity = 5, .orderCount = 1},
                            .ask = LevelSummary{.price = 101, .quantity = 3, .orderCount = 1}},
              }));
}

TEST_F(ExchangeTest, RejectsPostOnlyOrdersAtOrThroughTheOppositeBestPrice) {
    send(kBob, limitOrder(1, Side::Sell, 101, 3));
    send(kBob, limitOrder(2, Side::Buy, 99, 3));

    for (const auto& [side, price] : {std::pair{Side::Buy, Price{101}},
                                      std::pair{Side::Buy, Price{102}},
                                      std::pair{Side::Sell, Price{99}},
                                      std::pair{Side::Sell, Price{98}}}) {
        EXPECT_EQ(send(kAlice, limitOrder(1, side, price, 5, TimeInForce::PostOnly)),
                  (std::vector<Event>{OrderRejected{.agent = kAlice,
                                                    .clientOrderId = 1,
                                                    .request = RequestKind::New,
                                                    .reason = RejectReason::PostOnlyWouldTrade}}));
    }
    EXPECT_EQ(exchange_.account(kAlice), Account{});
}

TEST_F(ExchangeTest, PostOnlyOrderAgainstTheAgentsOwnOrderIsRejected) {
    send(kAlice, limitOrder(1, Side::Sell, 101, 3));

    // Self-trade prevention would cancel the incoming order; a post-only order never gets there.
    EXPECT_EQ(send(kAlice, limitOrder(2, Side::Buy, 101, 1, TimeInForce::PostOnly)),
              (std::vector<Event>{OrderRejected{.agent = kAlice,
                                                .clientOrderId = 2,
                                                .request = RequestKind::New,
                                                .reason = RejectReason::PostOnlyWouldTrade}}));
}

TEST_F(ExchangeTest, ModifyCannotMakeAPostOnlyOrderTrade) {
    send(kAlice, limitOrder(1, Side::Buy, 99, 2, TimeInForce::PostOnly));
    send(kBob, limitOrder(1, Side::Sell, 101, 3));
    const Event rejected = OrderRejected{.agent = kAlice,
                                         .clientOrderId = 1,
                                         .request = RequestKind::Modify,
                                         .reason = RejectReason::PostOnlyWouldTrade};

    EXPECT_EQ(send(kAlice, ModifyOrder{.clientOrderId = 1, .price = 101, .quantity = 2}),
              std::vector<Event>{rejected});
    EXPECT_EQ(exchange_.book().find(1), (RestingOrder{.id = 1,
                                                      .owner = kAlice,
                                                      .side = Side::Buy,
                                                      .price = 99,
                                                      .remaining = 2}));

    // A modify that keeps it passive goes through, and it stays post-only afterwards.
    EXPECT_EQ(send(kAlice, ModifyOrder{.clientOrderId = 1, .price = 100, .quantity = 2}).front(),
              (Event{OrderModified{.agent = kAlice,
                                   .clientOrderId = 1,
                                   .orderId = 1,
                                   .price = 100,
                                   .quantity = 2}}));
    EXPECT_EQ(send(kAlice, ModifyOrder{.clientOrderId = 1, .price = 102, .quantity = 2}),
              std::vector<Event>{rejected});
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

TEST(ExchangeFeeTest, ChargesMakersAndTakersTheirOwnRateOnEveryFill) {
    Exchange exchange{{.makerFee = -200, .takerFee = 300}}; // a 0.2 tick rebate, a 0.3 tick fee
    exchange.addAgent(kAlice);
    exchange.addAgent(kBob);
    std::vector<Event> events;
    exchange.handle(kBob, limitOrder(1, Side::Sell, 101, 3), events);
    events.clear();
    exchange.handle(kAlice, limitOrder(1, Side::Buy, 101, 5), events);

    ASSERT_GE(events.size(), 3U);
    EXPECT_EQ(std::get<OrderFilled>(events[1]).fee, -600);
    EXPECT_EQ(std::get<OrderFilled>(events[2]).fee, 900);
    EXPECT_EQ(exchange.account(kBob).fees, -600);
    EXPECT_EQ(exchange.account(kAlice).fees, 900);
    EXPECT_EQ(exchange.feesCollected(), 300);
    // Fees are kept apart from cash, which trading alone moves.
    EXPECT_EQ(exchange.account(kAlice).cash, -303);
    EXPECT_EQ(exchange.audit(), std::nullopt);
}

TEST(ExchangeFeeTest, RejectsRebatesLargerThanFeesAndOutlandishRates) {
    EXPECT_NO_THROW((Exchange{{.makerFee = -300, .takerFee = 300}}));
    EXPECT_THROW((Exchange{{.makerFee = -301, .takerFee = 300}}), std::invalid_argument);
    EXPECT_THROW((Exchange{{.takerFee = kMaxFeeRate + 1}}), std::invalid_argument);
}

class ExchangeDepthTest : public ::testing::Test {
protected:
    ExchangeDepthTest() {
        exchange_.addAgent(kAlice);
        exchange_.addAgent(kBob);
    }

    void TearDown() override { EXPECT_EQ(exchange_.audit(), std::nullopt); }

    std::vector<Event> send(AgentId agent, const Request& request) {
        std::vector<Event> events;
        exchange_.handle(agent, request, events);
        return events;
    }

    Exchange exchange_{{.depthLevels = 2}};
};

TEST_F(ExchangeDepthTest, PublishesTheBestLevelsWhenTheyChange) {
    send(kAlice, limitOrder(1, Side::Buy, 99, 5));
    EXPECT_EQ(send(kAlice, limitOrder(2, Side::Buy, 98, 3)).back(),
              (Event{BookDepth{.bids = {{.price = 99, .quantity = 5, .orderCount = 1},
                                        {.price = 98, .quantity = 3, .orderCount = 1}}}}));

    // A third level is deeper than the feed goes, so nothing published changes.
    EXPECT_EQ(send(kAlice, limitOrder(3, Side::Buy, 97, 1)).size(), 1U);

    // The depth update comes last, after the top of book.
    const std::vector<Event> events = send(kBob, limitOrder(1, Side::Sell, 101, 4));
    ASSERT_EQ(events.size(), 3U);
    EXPECT_TRUE(std::holds_alternative<TopOfBook>(events[1]));
    const BookDepth expected{.bids = {{.price = 99, .quantity = 5, .orderCount = 1},
                                      {.price = 98, .quantity = 3, .orderCount = 1}},
                             .asks = {{.price = 101, .quantity = 4, .orderCount = 1}}};
    EXPECT_EQ(events[2], Event{expected});

    // Taking out the best bid brings the third level into view.
    EXPECT_EQ(std::get<BookDepth>(send(kBob, marketOrder(2, Side::Sell, 5)).back()).bids,
              (std::vector<LevelSummary>{{.price = 98, .quantity = 3, .orderCount = 1},
                                         {.price = 97, .quantity = 1, .orderCount = 1}}));
}

TEST(ExchangeDepthSetupTest, PublishesNoDepthWithoutAFeedAndCapsItsDepth) {
    Exchange exchange;
    exchange.addAgent(kAlice);
    std::vector<Event> events;
    exchange.handle(kAlice, limitOrder(1, Side::Buy, 99, 5), events);
    const auto isDepth = [](const Event& event) {
        return std::holds_alternative<BookDepth>(event);
    };
    EXPECT_EQ(std::ranges::count_if(events, isDepth), 0);
    EXPECT_THROW((Exchange{{.depthLevels = kMaxDepthLevels + 1}}), std::invalid_argument);
}

TEST(FeeTest, FormatsFeesExactlyInTickLots) {
    EXPECT_EQ(formatFee(0), "0");
    EXPECT_EQ(formatFee(1'500), "1.5");
    EXPECT_EQ(formatFee(-250), "-0.25");
    EXPECT_EQ(formatFee(7), "0.007");
    EXPECT_EQ(formatFee(-12'000), "-12");
    EXPECT_EQ(formatFee(1'234), "1.234");
    EXPECT_EQ(formatFee(-1'010), "-1.01");
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

TEST(ExchangeSetupTest, HaltsLastFromANanosecondToTheLongestDuration) {
    EXPECT_THROW(Exchange(ExchangeConfig{.haltBand = 5, .haltDuration = 0}), std::invalid_argument);
    EXPECT_NO_THROW(Exchange(ExchangeConfig{.haltBand = 5, .haltDuration = kMaxDuration}));
    EXPECT_THROW(Exchange(ExchangeConfig{.haltBand = 5, .haltDuration = kMaxDuration + 1}),
                 std::invalid_argument);
}

TEST(AccountTest, EquityMarksThePositionToMarket) {
    EXPECT_EQ((Account{.cash = -303, .position = 3}).equity(105), 12);
    EXPECT_EQ((Account{.cash = 303, .position = -3}).equity(105), -12);
}

// A prediction market's shares pay 100 at most, so its exchange takes no price above 99, for new
// orders and modifies alike.
TEST(ExchangeLimitTest, TakesNoPriceAboveTheHighest) {
    Exchange exchange{ExchangeConfig{.maxPrice = 99}};
    exchange.addAgent(kAlice);
    std::vector<Event> events;
    exchange.handle(kAlice, limitOrder(1, Side::Buy, 100, 5), events);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(std::get<OrderRejected>(events[0]).reason, RejectReason::InvalidPrice);
    events.clear();
    exchange.handle(kAlice, limitOrder(2, Side::Sell, 99, 5), events);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(events.front()));
    events.clear();
    exchange.handle(kAlice, ModifyOrder{.clientOrderId = 2, .price = 100, .quantity = 5}, events);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(std::get<OrderRejected>(events[0]).reason, RejectReason::InvalidPrice);
    EXPECT_EQ(exchange.audit(), std::nullopt);

    EXPECT_THROW(Exchange(ExchangeConfig{.maxPrice = 0}), std::invalid_argument);
    EXPECT_THROW(Exchange(ExchangeConfig{.maxPrice = kMaxPrice + 1}), std::invalid_argument);
    EXPECT_THROW(Exchange(ExchangeConfig{.referencePrice = 100, .maxPrice = 99}),
                 std::invalid_argument);
}

} // namespace
} // namespace crowdbook

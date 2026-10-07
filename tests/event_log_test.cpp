#include <cstddef>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "crowdbook/event_log.hpp"

namespace crowdbook {
namespace {

constexpr std::size_t kColumns = 18;

// Joins fields into one CSV line, padding with empty trailing columns.
std::string row(std::initializer_list<std::string_view> fields) {
    std::string line;
    std::size_t count = 0;
    for (const std::string_view field : fields) {
        line += count++ == 0 ? "" : ",";
        line += field;
    }
    for (; count < kColumns; ++count) {
        line += ',';
    }
    return line + '\n';
}

TEST(CsvEventLogTest, WritesAHeaderAndOneRowPerRequestOrEvent) {
    std::ostringstream out;
    CsvEventLog log{out};

    log.onRequest(100, 1,
                  NewOrder{.clientOrderId = 7, .side = Side::Buy, .price = 99, .quantity = 5});
    log.onEvent(100, OrderAccepted{.agent = 1,
                                   .clientOrderId = 7,
                                   .orderId = 3,
                                   .side = Side::Buy,
                                   .price = 99,
                                   .quantity = 5});
    log.onRequest(150, 2,
                  NewOrder{.clientOrderId = 1,
                           .side = Side::Sell,
                           .type = OrderType::Market,
                           .quantity = 2});
    log.onEvent(150, OrderFilled{.agent = 1,
                                 .clientOrderId = 7,
                                 .orderId = 3,
                                 .side = Side::Buy,
                                 .price = 99,
                                 .quantity = 2,
                                 .leavesQuantity = 3,
                                 .liquidity = Liquidity::Maker});
    log.onEvent(150, OrderFilled{.agent = 2,
                                 .clientOrderId = 1,
                                 .orderId = 4,
                                 .side = Side::Sell,
                                 .price = 99,
                                 .quantity = 2,
                                 .leavesQuantity = 0,
                                 .liquidity = Liquidity::Taker});
    log.onEvent(150, Trade{.price = 99, .quantity = 2, .aggressorSide = Side::Sell});
    log.onEvent(150, TopOfBook{.bid = LevelSummary{.price = 99, .quantity = 3, .orderCount = 1}});
    log.onRequest(200, 1, CancelOrder{.clientOrderId = 7});
    log.onEvent(200, OrderCancelled{.agent = 1,
                                    .clientOrderId = 7,
                                    .orderId = 3,
                                    .quantity = 3,
                                    .reason = CancelReason::Requested});
    log.onRequest(250, 1, ModifyOrder{.clientOrderId = 8, .price = 98, .quantity = 1});
    log.onEvent(250, OrderRejected{.agent = 1,
                                   .clientOrderId = 8,
                                   .request = RequestKind::Modify,
                                   .reason = RejectReason::UnknownOrderId});

    const std::string expected =
        row({"time", "kind", "agent", "client_order_id", "order_id", "side", "type",
             "time_in_force", "price", "quantity", "leaves", "liquidity", "request", "reason",
             "bid_price", "bid_quantity", "ask_price", "ask_quantity"}) +
        row({"100", "new", "1", "7", "", "buy", "limit", "good-till-cancel", "99", "5"}) +
        row({"100", "accepted", "1", "7", "3", "buy", "limit", "good-till-cancel", "99", "5"}) +
        row({"150", "new", "2", "1", "", "sell", "market", "", "", "2"}) +
        row({"150", "filled", "1", "7", "3", "buy", "", "", "99", "2", "3", "maker"}) +
        row({"150", "filled", "2", "1", "4", "sell", "", "", "99", "2", "0", "taker"}) +
        row({"150", "trade", "", "", "", "sell", "", "", "99", "2"}) +
        row({"150", "top_of_book", "", "", "", "", "", "", "", "", "", "", "", "", "99", "3"}) +
        row({"200", "cancel", "1", "7"}) +
        row({"200", "cancelled", "1", "7", "3", "", "", "", "", "3", "", "", "", "requested"}) +
        row({"250", "modify", "1", "8", "", "", "", "", "98", "1"}) +
        row({"250", "rejected", "1", "8", "", "", "", "", "", "", "", "", "modify",
             "unknown order id"});
    EXPECT_EQ(out.str(), expected);
}

TEST(CsvEventLogTest, KeepsOnlyTheRequestedKinds) {
    std::ostringstream out;
    CsvEventLog log{out, {"trade", "top_of_book"}};
    log.onRequest(150, 2,
                  NewOrder{.clientOrderId = 1,
                           .side = Side::Sell,
                           .type = OrderType::Market,
                           .quantity = 2});
    log.onEvent(150, Trade{.price = 99, .quantity = 2, .aggressorSide = Side::Sell});
    log.onEvent(150, OrderCancelled{.agent = 2, .clientOrderId = 1, .orderId = 4, .quantity = 1});
    log.onEvent(150, TopOfBook{});

    const std::string text = out.str();
    EXPECT_EQ(text.substr(text.find('\n') + 1),
              row({"150", "trade", "", "", "", "sell", "", "", "99", "2"}) +
                  row({"150", "top_of_book"}));
}

TEST(CsvEventLogTest, RejectsUnknownKinds) {
    std::ostringstream out;
    EXPECT_THROW((CsvEventLog{out, {"trades"}}), std::invalid_argument);
}

TEST(PriceSamplerTest, WritesTheMarketAsOfEachInterval) {
    std::ostringstream out;
    PriceSampler sampler{out, 100};
    sampler.onEvent(0, TopOfBook{.bid = LevelSummary{.price = 99, .quantity = 1}});
    sampler.onEvent(150, TopOfBook{.bid = LevelSummary{.price = 99, .quantity = 1},
                                   .ask = LevelSummary{.price = 101, .quantity = 1}});
    sampler.onEvent(200, Trade{.price = 101, .quantity = 1});
    sampler.onRequest(390, 1, CancelOrder{.clientOrderId = 1});
    sampler.finish(400);

    // A row shows everything up to and including its time: the trade at 200 is in the 200 row.
    EXPECT_EQ(out.str(), "time,bid,ask,last_trade\n"
                         "0,99,,\n"
                         "100,99,,\n"
                         "200,99,101,101\n"
                         "300,99,101,101\n"
                         "400,99,101,101\n");
    EXPECT_THROW((PriceSampler{out, 0}), std::invalid_argument);
}

TEST(BroadcastSinkTest, PassesEverythingToEachSink) {
    std::ostringstream first;
    std::ostringstream second;
    CsvEventLog firstLog{first};
    CsvEventLog secondLog{second, {"trade"}};
    BroadcastSink broadcast;
    broadcast.add(firstLog);
    broadcast.add(secondLog);
    broadcast.onRequest(5, 1, CancelOrder{.clientOrderId = 2});
    broadcast.onEvent(5, Trade{.price = 99, .quantity = 1});

    EXPECT_NE(first.str().find(",cancel,"), std::string::npos);
    EXPECT_NE(first.str().find(",trade,"), std::string::npos);
    EXPECT_EQ(second.str().find(",cancel,"), std::string::npos);
    EXPECT_NE(second.str().find(",trade,"), std::string::npos);
}

} // namespace
} // namespace crowdbook

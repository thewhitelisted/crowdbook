#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/protocol.hpp"
#include "crowdbook/random.hpp"

namespace crowdbook::protocol {
namespace {

std::vector<ClientMessage> clientMessages() {
    return {
        Hello{.seat = "alice"},
        Hello{.seat = "Bot_2-b", .token = "s3cret!"},
        NewOrder{.clientOrderId = 17,
                 .side = Side::Buy,
                 .type = OrderType::Limit,
                 .timeInForce = TimeInForce::PostOnly,
                 .price = 10'003,
                 .quantity = 5},
        NewOrder{.clientOrderId = 18,
                 .side = Side::Sell,
                 .type = OrderType::Market,
                 .quantity = 2,
                 .parent = 7},
        NewOrder{.clientOrderId = 19,
                 .side = Side::Sell,
                 .type = OrderType::Limit,
                 .timeInForce = TimeInForce::ImmediateOrCancel,
                 .price = -4,
                 .quantity = 0},
        CancelOrder{.clientOrderId = 17},
        ModifyOrder{.clientOrderId = 17, .price = 10'004, .quantity = 3},
    };
}

std::vector<ServerMessage> serverMessages() {
    Welcome welcome{.seat = "alice",
                    .started = true,
                    .time = 1'500,
                    .duration = 60'000'000'000,
                    .referencePrice = 10'000,
                    .depthLevels = 10,
                    .makerFee = -200,
                    .takerFee = 300,
                    .latency = {.toExchange = 1'000'000, .fromExchange = 2'000'000, .jitter = 5},
                    .account = {.initialCash = 100,
                                .initialPosition = -3,
                                .cash = 90,
                                .position = 2,
                                .fees = 1'000,
                                .maxPosition = 50,
                                .maxOrderQuantity = 10}};
    welcome.orders.push_back({.clientOrderId = 4,
                              .orderId = 99,
                              .side = Side::Sell,
                              .type = OrderType::Limit,
                              .price = 10'010,
                              .leaves = 3,
                              .acknowledged = true,
                              .cancelRequested = true});
    TopOfBook top;
    top.bid = LevelSummary{.price = 10'003, .quantity = 12, .orderCount = 3};
    BookDepth depth;
    depth.bids = {{.price = 10'003, .quantity = 12, .orderCount = 3},
                  {.price = 10'002, .quantity = 1, .orderCount = 1}};
    return {
        welcome,
        Welcome{.seat = "bob"},
        Start{.time = 0},
        Clock{.time = 5'000'000'000},
        MarketMessage{.time = 1,
                      .event = OrderAccepted{.clientOrderId = 17,
                                             .orderId = 4'211,
                                             .side = Side::Buy,
                                             .type = OrderType::Limit,
                                             .timeInForce = TimeInForce::GoodTillCancel,
                                             .price = 10'003,
                                             .quantity = 5}},
        MarketMessage{.time = 2,
                      .event = OrderAccepted{.clientOrderId = 18,
                                             .orderId = 4'212,
                                             .side = Side::Sell,
                                             .type = OrderType::Market,
                                             .quantity = 2}},
        MarketMessage{.time = 3,
                      .event = OrderRejected{.clientOrderId = 17,
                                             .request = RequestKind::Modify,
                                             .reason = RejectReason::PostOnlyWouldTrade}},
        MarketMessage{.time = 4,
                      .event = OrderModified{
                          .clientOrderId = 17, .orderId = 4'211, .price = 10'004, .quantity = 3}},
        MarketMessage{.time = 5,
                      .event = OrderFilled{.clientOrderId = 17,
                                           .orderId = 4'211,
                                           .side = Side::Buy,
                                           .price = 10'004,
                                           .quantity = 2,
                                           .leavesQuantity = 1,
                                           .liquidity = Liquidity::Taker,
                                           .fee = -150}},
        MarketMessage{.time = 6,
                      .event = OrderCancelled{.clientOrderId = 17,
                                              .orderId = 4'211,
                                              .quantity = 1,
                                              .reason = CancelReason::SelfTrade}},
        MarketMessage{.time = 7,
                      .event = Trade{.price = 10'004, .quantity = 2, .aggressorSide = Side::Sell}},
        MarketMessage{.time = 8, .event = top},
        MarketMessage{.time = 9, .event = TopOfBook{}},
        MarketMessage{.time = 10, .event = depth},
        MarketMessage{.time = 11, .event = BookDepth{}},
        End{.time = 60'000'000'000, .cash = -50'012, .position = 5, .fees = 1'000, .pnl = 45},
        Error{.message = "unknown field 'qty'\n\"quoted\"", .fatal = true},
    };
}

TEST(Protocol, ClientMessagesRoundTrip) {
    for (const ClientMessage& message : clientMessages()) {
        const std::string line = encode(message);
        EXPECT_TRUE(line.ends_with('\n'));
        EXPECT_EQ(line.find('\n'), line.size() - 1) << line;
        EXPECT_EQ(decodeClient(line), message) << line;
    }
}

TEST(Protocol, ServerMessagesRoundTrip) {
    for (const ServerMessage& message : serverMessages()) {
        const std::string line = encode(message);
        EXPECT_EQ(line.find('\n'), line.size() - 1) << line;
        EXPECT_EQ(decodeServer(line), message) << line;
    }
}

TEST(Protocol, EncodesAsTheSpecificationShows) {
    EXPECT_EQ(encode(clientMessages()[2]),
              R"({"type":"new","id":17,"side":"buy","order_type":"limit",)"
              R"("time_in_force":"post-only","price":10003,"quantity":5})"
              "\n");
    EXPECT_EQ(encode(ServerMessage{MarketMessage{
                  .time = 3'000'000,
                  .event = Trade{.price = 10'004, .quantity = 2, .aggressorSide = Side::Sell}}}),
              R"({"type":"trade","time":3000000,"price":10004,"quantity":2,"aggressor":"sell"})"
              "\n");
}

TEST(Protocol, DecodesTheSpecificationsExamples) {
    EXPECT_NO_THROW(static_cast<void>(decodeClient(
        R"({"type":"hello","protocol":1,"seat":"alice","token":"s3cret"})")));
    EXPECT_NO_THROW(static_cast<void>(decodeServer(
        R"({"type":"welcome","protocol":1,"seat":"alice","started":false,"time":0,)"
        R"("duration":60000000000,"reference_price":10000,"depth_levels":10,"maker_fee":0,)"
        R"("taker_fee":0,"latency":{"to_exchange":1000000,"from_exchange":1000000,"jitter":0},)"
        R"("account":{"initial_cash":0,"initial_position":0,"cash":0,"position":0,"fees":0,)"
        R"("max_position":50,"max_order_quantity":10},"orders":[]})")));
    EXPECT_NO_THROW(static_cast<void>(decodeServer(
        R"({"type":"top","time":3000000,"bid":{"price":10003,"quantity":12,"orders":3},)"
        R"("ask":null})")));
}

TEST(Protocol, ClientMessagesAreChecked) {
    for (const std::string_view line : {
             R"({"type":"new","id":17,"side":"buy","order_type":"limit","price":1,"quantity":1})",
             R"({"type":"new","id":17,"side":"buy","order_type":"market","price":1,"quantity":1})",
             R"({"type":"new","id":0,"side":"buy","order_type":"market","quantity":1})",
             R"({"type":"new","id":-1,"side":"buy","order_type":"market","quantity":1})",
             R"({"type":"new","id":1,"side":"long","order_type":"market","quantity":1})",
             R"({"type":"new","id":1,"side":"buy","order_type":"market","quantity":"1"})",
             R"({"type":"new","id":1,"side":"buy","order_type":"market","quantity":1,"parent":-2})",
             R"({"type":"new","id":1,"side":"buy","order_type":"market","quantity":1,"qty":1})",
             R"({"type":"cancel"})",
             R"({"type":"cancel","id":1,"id":2})",
             R"({"type":"modify","id":1,"price":5})",
             R"({"type":"hello","protocol":1})",
             R"({"type":"hello","protocol":1,"seat":""})",
             R"({"type":"hello","protocol":1,"seat":"a b"})",
             R"({"type":"hello","protocol":1,"seat":"abcdefghijklmnopqrstuvwxyz0123456"})",
             R"({"type":"hello","protocol":1,"seat":"a","token":"has space"})",
             R"({"type":"hello","protocol":1,"seat":"a","token":""})",
             R"({"type":"welcome"})",
             R"({"type":"ping"})",
             R"({"id":1})",
             R"([1])",
             R"("hello")",
             R"({"type":1})",
         }) {
        EXPECT_THROW(static_cast<void>(decodeClient(line)), ProtocolError) << line;
    }
}

TEST(Protocol, SeatNames) {
    EXPECT_TRUE(validSeat("a"));
    EXPECT_TRUE(validSeat("Team-7_b"));
    EXPECT_TRUE(validSeat(std::string(kMaxSeatLength, 'x')));
    EXPECT_FALSE(validSeat(""));
    EXPECT_FALSE(validSeat(std::string(kMaxSeatLength + 1, 'x')));
    EXPECT_FALSE(validSeat("a.b"));
    EXPECT_FALSE(validSeat("caf\xc3\xa9"));
}

TEST(Protocol, RejectReasonsHaveWireNames) {
    EXPECT_EQ(wireName(RejectReason::PositionLimit), "position-limit");
    EXPECT_EQ(wireName(RejectReason::DuplicateClientOrderId), "duplicate-client-order-id");
}

// Malformed input from the network must be decoded or rejected with ProtocolError, never crash,
// and whatever decodes must encode to a line that decodes to the same message.
TEST(Protocol, SurvivesRandomlyMangledInput) {
    std::vector<std::string> lines;
    for (const ClientMessage& message : clientMessages()) {
        lines.push_back(encode(message));
    }
    std::size_t decoded = 0;
    std::size_t rejected = 0;
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        Random random{seed, 0};
        for (int i = 0; i < 2'000; ++i) {
            std::string line = lines[random.below(lines.size())];
            const auto mutations = random.uniformInt(1, 4);
            for (std::int64_t m = 0; m < mutations && !line.empty(); ++m) {
                const auto at = static_cast<std::size_t>(random.below(line.size()));
                const auto byte = static_cast<char>(random.below(256));
                switch (random.below(5)) {
                case 0:
                    line[at] = byte;
                    break;
                case 1:
                    line.erase(at, 1);
                    break;
                case 2:
                    line.insert(at, 1, byte);
                    break;
                case 3:
                    line.resize(at);
                    break;
                default: {
                    // Repeat a piece of the line, which makes repeated fields and deep nesting.
                    const auto length =
                        static_cast<std::size_t>(random.below(line.size() - at)) + 1;
                    line.insert(at, line.substr(at, length));
                    break;
                }
                }
            }
            try {
                const ClientMessage message = decodeClient(line);
                EXPECT_EQ(decodeClient(encode(message)), message) << line;
                ++decoded;
            } catch (const ProtocolError&) {
                ++rejected;
            }
        }
    }
    // Both outcomes have to be exercised for the test to mean anything.
    EXPECT_GT(decoded, 300U);
    EXPECT_GT(rejected, 10'000U);
}

} // namespace
} // namespace crowdbook::protocol

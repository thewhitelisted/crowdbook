#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/gateway.hpp"
#include "crowdbook/protocol.hpp"
#include "crowdbook/scenario_file.hpp"
#include "trading_screen.hpp"

namespace crowdbook {
namespace {

const Key kUp{.kind = Key::Kind::Up};

Key character(char c) {
    return {.kind = Key::Kind::Character, .character = c};
}

protocol::Welcome welcome(bool started, std::size_t depthLevels = 4) {
    return {.seat = "ana",
            .started = started,
            .duration = 30 * kSecond,
            .referencePrice = 1000,
            .depthLevels = depthLevels,
            .account = {.initialCash = 500, .cash = 500, .maxPosition = 40,
                        .maxOrderQuantity = 3}};
}

void receive(TradingScreen& screen, const protocol::ServerMessage& message) {
    screen.receive(protocol::encode(message));
}

// Presses a key and returns what it sent.
std::vector<protocol::ClientMessage> press(TradingScreen& screen, const Key& key) {
    std::vector<protocol::ClientMessage> sent;
    EXPECT_TRUE(screen.press(key, sent));
    return sent;
}

TEST(TradingScreenTest, SendsNothingUntilTheMarketStarts) {
    TradingScreen screen{"ana"};
    EXPECT_EQ(screen.screen(24, 80, true).message, "waiting for the market");
    EXPECT_TRUE(press(screen, character('b')).empty());

    receive(screen, welcome(false));
    EXPECT_EQ(screen.screen(24, 80, true).message, "waiting for every seat to be claimed");
    EXPECT_TRUE(press(screen, character('b')).empty());

    receive(screen, protocol::Start{.time = 0});
    EXPECT_EQ(press(screen, character('b')).size(), 1U);
}

TEST(TradingScreenTest, KeysBecomeOrdersAtTheCursor) {
    TradingScreen screen{"ana"};
    receive(screen, welcome(true));
    // The cursor starts at the reference price, the size at five lots or the most allowed.
    EXPECT_EQ(screen.screen(24, 80, true).cursor, 1000);
    EXPECT_EQ(screen.screen(24, 80, true).size, 3);

    press(screen, character('-'));
    EXPECT_TRUE(press(screen, kUp).empty());
    std::vector<protocol::ClientMessage> sent = press(screen, character('s'));
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(std::get<NewOrder>(sent[0]), (NewOrder{.clientOrderId = 1,
                                                    .side = Side::Sell,
                                                    .type = OrderType::Limit,
                                                    .price = 1001,
                                                    .quantity = 2}));
    EXPECT_EQ(screen.screen(24, 80, true).message, "offer 2 at 1001");

    sent = press(screen, character('B'));
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(std::get<NewOrder>(sent[0]).type, OrderType::Market);
    EXPECT_EQ(screen.screen(24, 80, true).message, "buy 2 at the market");

    // c cancels the limit orders at the cursor, C every one; each is cancelled once.
    press(screen, Key{.kind = Key::Kind::Down});
    press(screen, character('b'));
    press(screen, kUp);
    sent = press(screen, character('c'));
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(std::get<CancelOrder>(sent[0]).clientOrderId, 1U);
    EXPECT_EQ(press(screen, character('C')).size(), 1U); // the bid, order 3
    EXPECT_TRUE(press(screen, character('C')).empty());
    EXPECT_EQ(screen.screen(24, 80, true).message, "cancelling 0 orders");
}

TEST(TradingScreenTest, SaysWhatTheServerSays) {
    TradingScreen screen{"ana"};
    receive(screen, welcome(true, 0));
    EXPECT_EQ(screen.screen(24, 80, true).message,
              "no depth feed in this market: the ladder shows only the best prices");
    receive(screen, protocol::Error{.message = "slow down"});
    EXPECT_EQ(screen.screen(24, 80, true).message, "server: slow down");
    screen.receive("{not json}\n");
    EXPECT_TRUE(screen.screen(24, 80, true).message.starts_with(
        "a message from the server made no sense"));

    EXPECT_EQ(screen.screen(24, 80, false).message,
              "the connection to the market is closed: press q");
    receive(screen, protocol::End{.time = 30 * kSecond, .pnl = 12});
    EXPECT_EQ(screen.screen(24, 80, false).message,
              "the session is over: pnl +12 before fees; press q");
    EXPECT_TRUE(press(screen, character('b')).empty());
}

TEST(TradingScreenTest, ControlsAreForWhoeverRunsTheMarket) {
    std::vector<std::string> calls;
    TradingScreen controlled{
        "ana", TradingScreen::Controls{
                   .changeSpeed = [&](bool faster) { calls.emplace_back(faster ? "]" : "["); },
                   .togglePause = [&] { calls.emplace_back(" "); },
                   .speed = [] { return 2.5; },
                   .paused = [] { return true; }}};
    for (const char c : {' ', ']', '['}) {
        press(controlled, character(c));
    }
    EXPECT_EQ(calls, (std::vector<std::string>{" ", "]", "["}));
    const ladder::Screen view = controlled.screen(24, 80, true);
    EXPECT_EQ(view.speed, 2.5);
    EXPECT_TRUE(view.paused);
    EXPECT_EQ(view.seat, "");

    // A screen without controls ignores those keys and shows its seat instead.
    TradingScreen joined{"ana"};
    for (const char c : {' ', ']', '['}) {
        EXPECT_TRUE(press(joined, character(c)).empty());
    }
    EXPECT_EQ(joined.screen(24, 80, true).seat, "ana");
    std::vector<protocol::ClientMessage> sent;
    EXPECT_FALSE(joined.press(character('q'), sent));
}

// What `crowdbook play` does: the screen trades through a gateway in the same process.
TEST(TradingScreenTest, TradesThroughAGatewayInTheSameProcess) {
    constexpr std::string_view kScenario = R"(seed = 5
duration = "10s"
reference_price = 1000

[participant]
account = { initial_cash = 0, max_position = 40, max_order_quantity = 10 }

[[agents]]
type = "zero_intelligence"
name = "noise"
count = 10

[[agents]]
type = "market_maker"
name = "maker"
)";
    Gateway gateway{parseScenario(kScenario), std::string{kScenario},
                    AgentRegistry::withBuiltIns(), GatewayOptions{}};
    TradingScreen screen{"you"};
    const ConnectionId connection = gateway.connect(0);
    const auto deliver = [&] {
        const std::string_view pending = gateway.pendingOutput(connection);
        std::size_t start = 0;
        for (std::size_t end = pending.find('\n'); end != std::string_view::npos;
             end = pending.find('\n', start)) {
            screen.receive(pending.substr(start, end - start + 1));
            start = end + 1;
        }
        gateway.consumeOutput(connection, start);
    };
    const auto act = [&](char c, std::int64_t wall) {
        for (const protocol::ClientMessage& message : press(screen, character(c))) {
            gateway.receive(connection, protocol::encode(message), wall);
        }
    };
    gateway.receive(connection, protocol::encode(protocol::Hello{.seat = "you"}), 0);
    std::int64_t wall = 0;
    for (; wall < 9 * kSecond; wall += 50 * kMillisecond) {
        gateway.advance(wall);
        deliver();
        if (wall % kSecond == 0) {
            act('m', wall);
            act(wall % (2 * kSecond) == 0 ? 'b' : 's', wall);
            act('B', wall);
        }
    }
    act('C', wall);
    gateway.stop(wall);
    deliver();

    ASSERT_TRUE(screen.remote().end().has_value());
    const SessionMarket& market = gateway.market();
    const Ledger& server = market.run.simulation().ledger(market.seats[0].agent);
    EXPECT_GE(gateway.session().actions.size(), 18U);
    EXPECT_NE(server.cash(), 0); // the market buys went through
    EXPECT_EQ(screen.remote().ledger().position(), server.position());
    EXPECT_EQ(screen.remote().ledger().cash(), server.cash());
    EXPECT_EQ(screen.remote().end()->position, server.position());
}

} // namespace
} // namespace crowdbook

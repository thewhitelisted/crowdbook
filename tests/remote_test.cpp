#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <gtest/gtest.h>

#include "crowdbook/gateway.hpp"
#include "crowdbook/protocol.hpp"
#include "crowdbook/scenario_file.hpp"
#include "remote.hpp"

namespace crowdbook {
namespace {

constexpr std::string_view kScenario = R"(seed = 11
duration = "30s"
reference_price = 1000

[exchange]
depth_levels = 4
maker_fee = -0.1
taker_fee = 0.3

[participant]
latency = { to_exchange = "1ms", from_exchange = "2ms" }
account = { initial_cash = 500, initial_position = 3, max_position = 40, max_order_quantity = 10 }

[[agents]]
type = "zero_intelligence"
name = "noise"
count = 10
limit_rate = 5.0
market_rate = 1.0

[[agents]]
type = "market_maker"
name = "maker"
)";

// The same market with a trading day: a 2 s opening auction and a 2 s closing auction.
const std::string kDay = std::string{kScenario} + R"(
[trading_day]
opening_auction = "2s"
closing_auction = "2s"
)";

// The market, 12 s long.
Scenario twelveSeconds(std::string_view text) {
    Scenario scenario = parseScenario(text);
    scenario.duration = 12 * kSecond;
    return scenario;
}

// A gateway with one seat and a client model of it, connected in-process.
struct Connected {
    explicit Connected(std::string_view scenario = kScenario)
        : gateway{twelveSeconds(scenario), std::string{scenario}, AgentRegistry::withBuiltIns(),
                  GatewayOptions{}} {
        send(protocol::Hello{.seat = "you"}, 0);
    }

    Gateway gateway;
    ConnectionId connection = gateway.connect(0);
    RemoteMarket remote;

    void send(const protocol::ClientMessage& message, std::int64_t wall) {
        gateway.receive(connection, protocol::encode(message), wall);
        deliver();
    }

    void advance(std::int64_t wall) {
        gateway.advance(wall);
        deliver();
    }

    // Everything the gateway sent, applied to the client's model.
    void deliver() {
        const std::string_view pending = gateway.pendingOutput(connection);
        std::size_t start = 0;
        while (start < pending.size()) {
            const std::size_t end = pending.find('\n', start);
            remote.apply(protocol::decodeServer(pending.substr(start, end - start + 1)));
            start = end + 1;
        }
        gateway.consumeOutput(connection, pending.size());
    }

    [[nodiscard]] const Seat& seat() const { return gateway.market().seats[0]; }
    [[nodiscard]] const Simulation& simulation() const {
        return gateway.market().run.simulation();
    }
};

void expectSameLedger(const Ledger& remote, const Ledger& server) {
    EXPECT_EQ(remote.cash(), server.cash());
    EXPECT_EQ(remote.position(), server.position());
    EXPECT_EQ(remote.fees(), server.fees());
    ASSERT_EQ(remote.orders().size(), server.orders().size());
    auto mine = remote.orders().begin();
    for (const auto& [internal, theirs] : server.orders()) {
        EXPECT_EQ(mine->second.side, theirs.side);
        EXPECT_EQ(mine->second.price, theirs.price);
        EXPECT_EQ(mine->second.leaves, theirs.leaves);
        EXPECT_EQ(mine->second.acknowledged, theirs.acknowledged);
        EXPECT_EQ(mine->second.cancelRequested, theirs.cancelRequested);
        ++mine;
    }
}

// Trades for `steps` steps of 37 ms from `wall`: resting orders on both sides, market orders,
// cancels and a modify. Returns the wall-clock time it got to.
std::int64_t tradeFor(Connected& client, std::int64_t wall, int steps) {
    for (int step = 0; step < steps; ++step) {
        wall += 37 * kMillisecond;
        RemoteMarket& remote = client.remote;
        const Price mid = remote.market().lastTrade.value_or(1'000);
        switch (step % 7) {
        case 0:
            client.send(remote.order(Side::Buy, OrderType::Limit, mid - 2, 2), wall);
            break;
        case 1:
            client.send(remote.order(Side::Sell, OrderType::Limit, mid + 2, 2), wall);
            break;
        case 2:
            client.send(remote.order(step % 2 == 0 ? Side::Buy : Side::Sell, OrderType::Market,
                                     0, 1),
                        wall);
            break;
        case 3:
            for (const CancelOrder& cancel : remote.cancels(mid - 2)) {
                client.send(cancel, wall);
            }
            break;
        case 4:
            if (!remote.ledger().orders().empty()) {
                const auto& [id, own] = *remote.ledger().orders().begin();
                if (own.type == OrderType::Limit && !own.cancelRequested) {
                    client.send(ModifyOrder{.clientOrderId = id,
                                            .price = own.price,
                                            .quantity = 1},
                                wall);
                }
            }
            break;
        default:
            client.advance(wall);
        }
    }
    return wall;
}

TEST(RemoteMarketTest, TheClientSeesWhatTheServerKnows) {
    Connected client;
    ASSERT_TRUE(client.remote.welcomed());
    EXPECT_TRUE(client.remote.started());
    EXPECT_EQ(client.remote.ledger().cash(), 500);
    EXPECT_EQ(client.remote.ledger().position(), 3);

    const std::int64_t wall = tradeFor(client, 0, 200);
    // Let everything in flight land.
    client.advance(wall + 50 * kMillisecond);
    ASSERT_FALSE(client.gateway.finished());
    EXPECT_GT(client.remote.tape().size(), 0U);
    EXPECT_LE(client.remote.now(), client.simulation().now()); // the last message it got
    expectSameLedger(client.remote.ledger(), client.simulation().ledger(client.seat().agent));
    EXPECT_EQ(client.remote.market(), client.simulation().marketSeenBy(client.seat().agent));
}

// Through a trading day too: auctions, where market orders are turned away, and the phase and
// the indicative price, which the client must see as the server does.
TEST(RemoteMarketTest, TheClientSeesTheTradingDayAsTheServerDoes) {
    Connected client{kDay};
    std::int64_t wall = 0;
    for (const auto& [steps, expected] :
         {std::pair{30, Phase::OpeningAuction}, std::pair{200, Phase::Continuous},
          std::pair{50, Phase::ClosingAuction}}) {
        wall = tradeFor(client, wall, steps) + 50 * kMillisecond;
        client.advance(wall);
        EXPECT_EQ(client.remote.market().phase, expected);
        expectSameLedger(client.remote.ledger(),
                         client.simulation().ledger(client.seat().agent));
        EXPECT_EQ(client.remote.market(),
                  client.simulation().marketSeenBy(client.seat().agent));
    }
    EXPECT_TRUE(client.remote.market().indicative.has_value());
}

TEST(RemoteMarketTest, AClientClaimingTheSeatAgainCarriesOn) {
    Connected client;
    client.send(client.remote.order(Side::Buy, OrderType::Market, 0, 2), kSecond);
    client.send(client.remote.order(Side::Sell, OrderType::Limit, 1'050, 3), kSecond);
    client.advance(2 * kSecond);
    client.send(client.remote.order(Side::Sell, OrderType::Limit, 1'060, 1), 2 * kSecond);

    // A new client takes over the seat while the last order is in flight.
    client.gateway.disconnect(client.connection, 2 * kSecond);
    client.connection = client.gateway.connect(2 * kSecond);
    client.remote = RemoteMarket{};
    client.send(protocol::Hello{.seat = "you"}, 2 * kSecond);
    expectSameLedger(client.remote.ledger(), client.simulation().ledger(client.seat().agent));
    // Its next id does not collide with the orders it took over.
    const NewOrder next = client.remote.order(Side::Buy, OrderType::Limit, 900, 1);
    EXPECT_EQ(next.clientOrderId, 4U);
    client.send(next, 2 * kSecond);
    client.advance(3 * kSecond);
    expectSameLedger(client.remote.ledger(), client.simulation().ledger(client.seat().agent));
}

TEST(RemoteMarketTest, TellsThePersonAboutRejectionsErrorsAndTheEnd) {
    Connected client;
    client.send(client.remote.order(Side::Buy, OrderType::Limit, 900, 11), kSecond);
    client.advance(kSecond + 3 * kMillisecond);
    EXPECT_EQ(client.remote.message(), "new of order 1 rejected: order size limit");
    client.gateway.receive(client.connection, "nonsense\n", 2 * kSecond);
    client.deliver();
    EXPECT_TRUE(client.remote.message().starts_with("server: not valid JSON"));
    client.advance(40 * kSecond);
    ASSERT_TRUE(client.remote.end().has_value());
    EXPECT_TRUE(client.remote.message().starts_with("the session is over"));
}

} // namespace
} // namespace crowdbook

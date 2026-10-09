#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/gateway.hpp"
#include "crowdbook/protocol.hpp"
#include "crowdbook/scenario_file.hpp"
#include "test_agents.hpp"

namespace crowdbook {
namespace {

using protocol::ServerMessage;

// A busy little market with a depth feed, where a seat is a millisecond from the exchange.
constexpr std::string_view kScenario = R"(seed = 5
duration = "20s"
reference_price = 1000

[exchange]
depth_levels = 3

[participant]
latency = { to_exchange = "1ms", from_exchange = "1ms" }
account = { max_position = 50, max_order_quantity = 10 }

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

// A market where nothing trades, so nothing is published unless the seats trade.
constexpr std::string_view kQuietScenario = R"(duration = "20s"

[participant]
latency = { to_exchange = "1ms", from_exchange = "1ms" }

[[agents]]
type = "momentum"
)";

constexpr std::int64_t kMs = kMillisecond;

// One client of a gateway: sends lines and reads what the gateway sends back.
class Client {
public:
    Client(Gateway& gateway, std::int64_t wallNow)
        : gateway_(gateway), id_(gateway.connect(wallNow)) {}

    [[nodiscard]] ConnectionId id() const noexcept { return id_; }

    void send(std::string_view line, std::int64_t wallNow) {
        gateway_.receive(id_, std::string{line} + "\n", wallNow);
    }
    void send(const protocol::ClientMessage& message, std::int64_t wallNow) {
        gateway_.receive(id_, protocol::encode(message), wallNow);
    }
    void hello(std::string_view seat, std::int64_t wallNow) {
        send(protocol::Hello{.seat = std::string{seat}}, wallNow);
    }

    // Everything sent since the last call, decoded.
    std::vector<ServerMessage> read() {
        const std::string_view pending = gateway_.pendingOutput(id_);
        std::vector<ServerMessage> messages;
        std::size_t start = 0;
        while (start < pending.size()) {
            const std::size_t end = pending.find('\n', start);
            EXPECT_NE(end, std::string_view::npos) << "a message without its newline";
            messages.push_back(protocol::decodeServer(pending.substr(start, end - start + 1)));
            start = end + 1;
        }
        gateway_.consumeOutput(id_, pending.size());
        return messages;
    }

private:
    Gateway& gateway_;
    ConnectionId id_;
};

template <typename Message>
std::vector<Message> only(const std::vector<ServerMessage>& messages) {
    std::vector<Message> found;
    for (const ServerMessage& message : messages) {
        if (const auto* body = std::get_if<Message>(&message)) {
            found.push_back(*body);
        }
    }
    return found;
}

// The order events in `messages`, of one kind.
template <typename Event>
std::vector<Event> events(const std::vector<ServerMessage>& messages) {
    std::vector<Event> found;
    for (const auto& market : only<protocol::MarketMessage>(messages)) {
        if (const auto* event = std::get_if<Event>(&market.event)) {
            found.push_back(*event);
        }
    }
    return found;
}

std::vector<protocol::Error> errors(const std::vector<ServerMessage>& messages) {
    return only<protocol::Error>(messages);
}

GatewayOptions twoSeats() {
    GatewayOptions options;
    options.seats = {"alice", "bob"};
    return options;
}

NewOrder bid(ClientOrderId id, Price price, Quantity quantity) {
    return {.clientOrderId = id,
            .side = Side::Buy,
            .type = OrderType::Limit,
            .price = price,
            .quantity = quantity};
}

NewOrder offer(ClientOrderId id, Price price, Quantity quantity) {
    return {.clientOrderId = id,
            .side = Side::Sell,
            .type = OrderType::Limit,
            .price = price,
            .quantity = quantity};
}

TEST(GatewayTest, TheClockStartsWhenEverySeatIsClaimed) {
    Gateway gateway{parseScenario(kScenario), std::string{kScenario},
                    AgentRegistry::withBuiltIns(), twoSeats()};
    Client alice{gateway, 0};
    alice.hello("alice", 0);
    const auto first = alice.read();
    ASSERT_EQ(first.size(), 1U);
    const auto& welcome = std::get<protocol::Welcome>(first[0]);
    EXPECT_EQ(welcome.seat, "alice");
    EXPECT_FALSE(welcome.started);
    EXPECT_EQ(welcome.duration, 20 * kSecond);
    EXPECT_EQ(welcome.referencePrice, 1'000);
    EXPECT_EQ(welcome.depthLevels, 3U);
    EXPECT_EQ(welcome.latency.toExchange, kMs);
    EXPECT_EQ(welcome.account.maxPosition, 50);
    EXPECT_EQ(welcome.account.maxOrderQuantity, 10);

    gateway.advance(3 * kSecond);
    EXPECT_FALSE(gateway.started());
    EXPECT_EQ(gateway.now(), 0);
    EXPECT_TRUE(alice.read().empty());

    Client bob{gateway, 3 * kSecond};
    bob.hello("bob", 3 * kSecond);
    EXPECT_TRUE(gateway.started());
    EXPECT_EQ(only<protocol::Start>(alice.read()).size(), 1U);
    const auto bobs = bob.read();
    ASSERT_EQ(bobs.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<protocol::Welcome>(bobs[0]));
    EXPECT_EQ(std::get<protocol::Start>(bobs[1]).time, 0);

    // One wall-clock second later, a simulated second has passed, and the market has traded.
    gateway.advance(4 * kSecond);
    EXPECT_EQ(gateway.now(), kSecond);
    EXPECT_FALSE(only<protocol::MarketMessage>(alice.read()).empty());
}

// Two seats claimed and the clock started at wall time 0.
struct Started {
    Gateway gateway;
    Client alice;
    Client bob;

    explicit Started(GatewayOptions options = twoSeats(),
                     std::string_view scenario = kScenario, EventSink* sink = nullptr)
        : gateway(parseScenario(scenario), std::string{scenario}, AgentRegistry::withBuiltIns(),
                  std::move(options), sink),
          alice(gateway, 0), bob(gateway, 0) {
        alice.hello("alice", 0);
        bob.hello("bob", 0);
        alice.read();
        bob.read();
    }
};

TEST(GatewayTest, OrdersKeepTheClientsIds) {
    Started market;
    market.alice.send(bid(100, 900, 2), kSecond);
    market.gateway.advance(kSecond + 2 * kMs);
    const auto accepted = events<OrderAccepted>(market.alice.read());
    ASSERT_EQ(accepted.size(), 1U);
    EXPECT_EQ(accepted[0].clientOrderId, 100U);
    EXPECT_EQ(accepted[0].agent, 0U);
    EXPECT_EQ(accepted[0].price, 900);

    market.alice.send(protocol::ClientMessage{ModifyOrder{.clientOrderId = 100,
                                                          .price = 901,
                                                          .quantity = 1}},
                      2 * kSecond);
    market.alice.send(protocol::ClientMessage{CancelOrder{.clientOrderId = 100}}, 2 * kSecond);
    market.gateway.advance(2 * kSecond + 2 * kMs);
    const auto messages = market.alice.read();
    const auto modified = events<OrderModified>(messages);
    ASSERT_EQ(modified.size(), 1U);
    EXPECT_EQ(modified[0].clientOrderId, 100U);
    const auto cancelled = events<OrderCancelled>(messages);
    ASSERT_EQ(cancelled.size(), 1U);
    EXPECT_EQ(cancelled[0].clientOrderId, 100U);
    EXPECT_EQ(cancelled[0].reason, CancelReason::Requested);

    // Done with, the id can name a new order.
    market.alice.send(bid(100, 899, 1), 3 * kSecond);
    market.gateway.advance(3 * kSecond + 2 * kMs);
    EXPECT_EQ(events<OrderAccepted>(market.alice.read()).size(), 1U);
    // Bob's orders are his own, and his ids may be the same as Alice's.
    market.bob.send(bid(100, 898, 1), 3 * kSecond + 2 * kMs);
    market.gateway.advance(3 * kSecond + 4 * kMs);
    EXPECT_EQ(events<OrderAccepted>(market.bob.read()).size(), 1U);
    EXPECT_TRUE(events<OrderAccepted>(market.alice.read()).empty());
}

TEST(GatewayTest, AnOrderCanBeCancelledBeforeItIsAcknowledged) {
    Started market;
    market.alice.send(bid(7, 900, 2), kSecond);
    market.alice.send(protocol::ClientMessage{CancelOrder{.clientOrderId = 7}}, kSecond);
    market.gateway.advance(kSecond + 2 * kMs);
    const auto messages = market.alice.read();
    EXPECT_EQ(events<OrderAccepted>(messages).size(), 1U);
    ASSERT_EQ(events<OrderCancelled>(messages).size(), 1U);
    EXPECT_EQ(events<OrderCancelled>(messages)[0].clientOrderId, 7U);
    EXPECT_TRUE(errors(messages).empty());
}

TEST(GatewayTest, TheGatewayRejectsIdsAtOnce) {
    Started market;
    market.alice.send(bid(7, 900, 2), kSecond);
    market.alice.send(bid(7, 901, 2), kSecond);
    market.alice.send(protocol::ClientMessage{CancelOrder{.clientOrderId = 8}}, kSecond);
    market.alice.send(protocol::ClientMessage{ModifyOrder{.clientOrderId = 9,
                                                          .price = 1,
                                                          .quantity = 1}},
                      kSecond);
    // Answered straight away, before the order itself could have reached the exchange.
    const auto rejected = events<OrderRejected>(market.alice.read());
    ASSERT_EQ(rejected.size(), 3U);
    EXPECT_EQ(rejected[0].clientOrderId, 7U);
    EXPECT_EQ(rejected[0].reason, RejectReason::DuplicateClientOrderId);
    EXPECT_EQ(rejected[1].clientOrderId, 8U);
    EXPECT_EQ(rejected[1].request, RequestKind::Cancel);
    EXPECT_EQ(rejected[1].reason, RejectReason::UnknownOrderId);
    EXPECT_EQ(rejected[2].request, RequestKind::Modify);
    // Only the first order reached the market.
    EXPECT_EQ(market.gateway.session().actions.size(), 1U);
}

TEST(GatewayTest, TheExchangesRejectionsArriveWithLatency) {
    Started market;
    market.alice.send(bid(1, 900, 11), kSecond); // over the largest order of ten
    EXPECT_TRUE(events<OrderRejected>(market.alice.read()).empty());
    market.gateway.advance(kSecond + 2 * kMs);
    const auto messages = only<protocol::MarketMessage>(market.alice.read());
    const auto found = std::ranges::find_if(messages, [](const protocol::MarketMessage& m) {
        return std::holds_alternative<OrderRejected>(m.event);
    });
    ASSERT_NE(found, messages.end());
    EXPECT_EQ(found->time, kSecond + 2 * kMs);
    EXPECT_EQ(std::get<OrderRejected>(found->event).reason, RejectReason::OrderSizeLimit);
}

// A session with two clients, with a disconnect and a seat claimed again, replays to the same
// event log byte for byte.
TEST(GatewayTest, ASessionReplaysToTheSameLogByteForByte) {
    std::ostringstream live;
    CsvEventLog sink{live};
    Started market{twoSeats(), kScenario, &sink};
    Gateway& gateway = market.gateway;
    // The clients act at uneven wall-clock times, some of them together.
    market.alice.send(bid(1, 1'001, 5), 1'013 * kMs);
    market.bob.send(offer(1, 1'001, 5), 1'013 * kMs);
    gateway.advance(1'500 * kMs);
    market.bob.send(offer(2, 1'030, 3), 2'200 * kMs + 7);
    market.alice.send(bid(2, 970, 4), 2'200 * kMs + 7);
    market.alice.send(protocol::ClientMessage{ModifyOrder{.clientOrderId = 2,
                                                          .price = 975,
                                                          .quantity = 2}},
                      2'900 * kMs);
    gateway.advance(3 * kSecond);
    // Bob drops out with an order open, which is cancelled, and comes back.
    gateway.disconnect(market.bob.id(), 3'100 * kMs);
    gateway.advance(4 * kSecond);
    Client bob{gateway, 4 * kSecond};
    bob.hello("bob", 4 * kSecond);
    const auto welcome = only<protocol::Welcome>(bob.read());
    ASSERT_EQ(welcome.size(), 1U);
    EXPECT_TRUE(welcome[0].started);
    EXPECT_TRUE(welcome[0].orders.empty());
    bob.send(NewOrder{.clientOrderId = 3,
                      .side = Side::Buy,
                      .type = OrderType::Market,
                      .quantity = 2},
             4'321 * kMs);
    gateway.advance(5 * kSecond);
    gateway.stop(6 * kSecond);
    EXPECT_TRUE(gateway.finished());
    EXPECT_EQ(gateway.session().end, 6 * kSecond);
    EXPECT_EQ(gateway.session().seats, (std::vector<std::string>{"alice", "bob"}));
    // Bob's order 2, which the disconnect cancelled, is in the recording as a cancel.
    EXPECT_EQ(gateway.session().actions.size(), 7U);

    std::ostringstream replayed;
    CsvEventLog replaySink{replayed};
    const RunResult result =
        replaySession(gateway.session(), AgentRegistry::withBuiltIns(), &replaySink);
    EXPECT_EQ(replayed.str(), live.str());
    EXPECT_GT(std::ranges::count(live.str(), '\n'), 500);
    const RunResult played = gateway.result();
    ASSERT_EQ(result.groups.size(), played.groups.size());
    for (std::size_t i = 0; i < result.groups.size(); ++i) {
        EXPECT_EQ(result.groups[i].pnl, played.groups[i].pnl);
    }
}

// A session stopped before every seat is claimed still runs what was due at time 0, such as the
// market maker's first quotes, as its replay does.
TEST(GatewayTest, ASessionStoppedBeforeItStartsReplaysTheSame) {
    std::ostringstream live;
    CsvEventLog sink{live};
    Gateway gateway{parseScenario(kScenario), std::string{kScenario},
                    AgentRegistry::withBuiltIns(), twoSeats(), &sink};
    Client alice{gateway, 0};
    alice.hello("alice", 0);
    gateway.advance(kSecond);
    gateway.stop(2 * kSecond);
    EXPECT_FALSE(gateway.started());
    EXPECT_EQ(gateway.session().end, 0);
    std::ostringstream replayed;
    CsvEventLog replaySink{replayed};
    static_cast<void>(replaySession(gateway.session(), AgentRegistry::withBuiltIns(), &replaySink));
    EXPECT_EQ(replayed.str(), live.str());
    EXPECT_GT(std::ranges::count(live.str(), '\n'), 1);
}

// Bob's cancel is on its way when Alice's order fills his offer: the answer to the cancel comes
// after the order is done, and still names it by Bob's id.
TEST(GatewayTest, AnAnswerAfterAnOrderIsDoneKeepsTheClientsId) {
    Started market{twoSeats(), kQuietScenario};
    market.bob.send(offer(1, 1'001, 2), kSecond);
    market.gateway.advance(kSecond + 10 * kMs);
    market.alice.send(NewOrder{.clientOrderId = 9,
                               .side = Side::Buy,
                               .type = OrderType::Market,
                               .quantity = 2},
                      2 * kSecond);
    market.bob.send(protocol::ClientMessage{CancelOrder{.clientOrderId = 1}},
                    2 * kSecond + kMs / 2);
    market.gateway.advance(2 * kSecond + 10 * kMs);
    const auto messages = market.bob.read();
    const auto filled = events<OrderFilled>(messages);
    ASSERT_EQ(filled.size(), 1U);
    EXPECT_EQ(filled[0].clientOrderId, 1U);
    const auto rejected = events<OrderRejected>(messages);
    ASSERT_EQ(rejected.size(), 1U);
    EXPECT_EQ(rejected[0].clientOrderId, 1U);
    EXPECT_EQ(rejected[0].request, RequestKind::Cancel);
    EXPECT_EQ(rejected[0].reason, RejectReason::UnknownOrderId);
    // And the id is free again.
    market.bob.send(offer(1, 1'002, 1), 3 * kSecond);
    market.gateway.advance(3 * kSecond + 10 * kMs);
    EXPECT_EQ(events<OrderAccepted>(market.bob.read()).size(), 1U);
}

TEST(GatewayTest, ADisconnectCancelsTheSeatsOrders) {
    Started market;
    market.alice.send(bid(1, 900, 2), kSecond);
    market.alice.send(bid(2, 901, 2), kSecond);
    market.gateway.advance(2 * kSecond);
    market.gateway.disconnect(market.alice.id(), 2 * kSecond);
    market.gateway.advance(3 * kSecond);
    const Seat& seat = market.gateway.market().seats[0];
    EXPECT_TRUE(market.gateway.market().run.simulation().ledger(seat.agent).orders().empty());
    const auto& actions = market.gateway.session().actions;
    ASSERT_EQ(actions.size(), 4U);
    EXPECT_TRUE(std::holds_alternative<CancelOrder>(actions[2].request));
    EXPECT_TRUE(std::holds_alternative<CancelOrder>(actions[3].request));
}

TEST(GatewayTest, ASeatClaimedAgainSeesItsOrdersByTheClientsIds) {
    Started market;
    market.alice.send(bid(41, 900, 2), kSecond);
    market.gateway.advance(2 * kSecond);
    // A second connection for a taken seat is turned away.
    Client again{market.gateway, 2 * kSecond};
    again.hello("alice", 2 * kSecond);
    const auto refused = errors(again.read());
    ASSERT_EQ(refused.size(), 1U);
    EXPECT_TRUE(refused[0].fatal);
    EXPECT_TRUE(market.gateway.shouldClose(again.id()));
    market.gateway.disconnect(again.id(), 2 * kSecond);
    // The seat's orders are cancelled when its connection goes, but the welcome on the way back
    // can still show the cancel in flight.
    market.gateway.disconnect(market.alice.id(), 2 * kSecond);
    Client back{market.gateway, 2 * kSecond};
    back.hello("alice", 2 * kSecond);
    const auto welcome = only<protocol::Welcome>(back.read());
    ASSERT_EQ(welcome.size(), 1U);
    ASSERT_EQ(welcome[0].orders.size(), 1U);
    EXPECT_EQ(welcome[0].orders[0].clientOrderId, 41U);
    EXPECT_TRUE(welcome[0].orders[0].cancelRequested);
    market.gateway.advance(2 * kSecond + 2 * kMs);
    const auto cancelled = events<OrderCancelled>(back.read());
    ASSERT_EQ(cancelled.size(), 1U);
    EXPECT_EQ(cancelled[0].clientOrderId, 41U);
}

TEST(GatewayTest, MalformedMessagesAreAnsweredAndDropped) {
    Started market;
    market.alice.send("{\"type\":\"new\",\"id\":1}", kSecond);
    market.alice.send("not json", kSecond);
    market.alice.send("", kSecond); // blank lines are ignored
    market.alice.send(std::string(protocol::kMaxLineLength + 10, 'x'), kSecond);
    market.alice.send(bid(1, 900, 1), kSecond); // still trading after all that
    const auto messages = market.alice.read();
    const auto complaints = errors(messages);
    ASSERT_EQ(complaints.size(), 3U);
    for (const auto& error : complaints) {
        EXPECT_FALSE(error.fatal);
    }
    EXPECT_NE(complaints[0].message.find("missing field"), std::string::npos);
    EXPECT_NE(complaints[2].message.find("longer than"), std::string::npos);
    EXPECT_FALSE(market.gateway.shouldClose(market.alice.id()));
    EXPECT_EQ(market.gateway.session().actions.size(), 1U);
}

TEST(GatewayTest, ALongLineSplitAcrossReadsIsDropped) {
    Started market;
    const std::string half(protocol::kMaxLineLength / 2 + 1, 'x');
    market.gateway.receive(market.alice.id(), half, kSecond);
    market.gateway.receive(market.alice.id(), half, kSecond);
    market.gateway.receive(market.alice.id(), half + "\n" + protocol::encode(bid(1, 900, 1)),
                           kSecond);
    EXPECT_EQ(errors(market.alice.read()).size(), 1U);
    EXPECT_EQ(market.gateway.session().actions.size(), 1U);
}

TEST(GatewayTest, AMessageSplitAcrossReadsIsPutBackTogether) {
    Started market;
    const std::string line = protocol::encode(bid(1, 900, 1));
    for (const char c : line) {
        market.gateway.receive(market.alice.id(), std::string(1, c), kSecond);
    }
    EXPECT_TRUE(errors(market.alice.read()).empty());
    EXPECT_EQ(market.gateway.session().actions.size(), 1U);
}

TEST(GatewayTest, ABadStartIsFatal) {
    GatewayOptions options = twoSeats();
    options.tokens = {{"alice", "red"}, {"bob", "blue"}};
    Gateway gateway{parseScenario(kScenario), std::string{kScenario},
                    AgentRegistry::withBuiltIns(), options};
    const std::vector<std::string> openings = {
        protocol::encode(bid(1, 900, 1)),
        "garbage\n",
        R"({"type":"hello","protocol":2,"seat":"alice","token":"red"})" "\n",
        R"({"type":"hello","protocol":1,"seat":"carol","token":"red"})" "\n",
        R"({"type":"hello","protocol":1,"seat":"alice","token":"blue"})" "\n",
        R"({"type":"hello","protocol":1,"seat":"alice"})" "\n",
    };
    for (const std::string& opening : openings) {
        Client client{gateway, 0};
        gateway.receive(client.id(), opening, 0);
        // Anything after a fatal error is ignored.
        gateway.receive(client.id(), R"({"type":"hello","protocol":1,"seat":"alice","token":"red"})"
                                     "\n",
                        0);
        const auto messages = client.read();
        ASSERT_EQ(messages.size(), 1U) << opening;
        EXPECT_TRUE(std::get<protocol::Error>(messages[0]).fatal) << opening;
        EXPECT_TRUE(gateway.shouldClose(client.id()));
        gateway.disconnect(client.id(), 0);
    }
    Client alice{gateway, 0};
    alice.send(R"({"type":"hello","protocol":1,"seat":"alice","token":"red"})", 0);
    EXPECT_EQ(only<protocol::Welcome>(alice.read()).size(), 1U);
}

TEST(GatewayTest, AConnectionHasToSayHelloInTime) {
    Gateway gateway{parseScenario(kScenario), std::string{kScenario},
                    AgentRegistry::withBuiltIns(), twoSeats()};
    Client silent{gateway, kSecond};
    gateway.advance(6 * kSecond - 1);
    EXPECT_FALSE(gateway.shouldClose(silent.id()));
    gateway.advance(6 * kSecond);
    const auto messages = silent.read();
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_TRUE(std::get<protocol::Error>(messages[0]).fatal);
    EXPECT_TRUE(gateway.shouldClose(silent.id()));
}

TEST(GatewayTest, MessagesOverTheRateLimitAreDropped) {
    GatewayOptions options = twoSeats();
    options.maxMessagesPerSecond = 4;
    Started market{options};
    // The hello used one; three more fit in the burst, the other two do not.
    for (ClientOrderId id = 1; id <= 5; ++id) {
        market.alice.send(bid(id, 900, 1), 0);
    }
    // Half a second later, two more are allowed.
    for (ClientOrderId id = 6; id <= 8; ++id) {
        market.alice.send(bid(id, 900, 1), kSecond / 2);
    }
    // Each order over the limit is rejected by its id, at once.
    const auto refused = events<OrderRejected>(market.alice.read());
    ASSERT_EQ(refused.size(), 3U);
    EXPECT_EQ(refused[0].clientOrderId, 4U);
    EXPECT_EQ(refused[0].reason, RejectReason::RateLimit);
    EXPECT_EQ(refused[2].clientOrderId, 8U);
    EXPECT_EQ(market.gateway.session().actions.size(), 5U);
    // A line over the limit that is not an order gets an error.
    for (int i = 0; i < 5; ++i) {
        market.alice.send("not json", kSecond / 2);
    }
    EXPECT_FALSE(errors(market.alice.read()).empty());
}

TEST(GatewayTest, AClientThatFallsBehindIsDropped) {
    GatewayOptions options = twoSeats();
    options.maxPendingOutput = 4'096;
    Started market{options};
    // Alice reads nothing, and a busy market soon outruns her limit. Bob keeps up.
    for (std::int64_t wall = 0; wall <= 5 * kSecond; wall += 50 * kMs) {
        market.gateway.advance(wall);
        market.bob.read();
    }
    EXPECT_TRUE(market.gateway.shouldClose(market.alice.id()));
    EXPECT_TRUE(market.gateway.pendingOutput(market.alice.id()).empty());
    EXPECT_FALSE(market.gateway.shouldClose(market.bob.id()));
}

TEST(GatewayTest, AQuietMarketStillTellsTheTime) {
    Started market{twoSeats(), kQuietScenario};
    market.gateway.advance(50 * kMs);
    EXPECT_TRUE(market.alice.read().empty());
    market.gateway.advance(100 * kMs);
    const auto clocks = only<protocol::Clock>(market.alice.read());
    ASSERT_EQ(clocks.size(), 1U);
    EXPECT_EQ(clocks[0].time, 100 * kMs);
}

// Whoever runs the gateway can pause its clock and change its speed; the market's time carries
// on from where it was. A pause asked for before the clock starts holds once it does.
TEST(GatewayTest, TheClockCanBePausedAndSpedUp) {
    Gateway gateway{parseScenario(kQuietScenario), std::string{kQuietScenario},
                    AgentRegistry::withBuiltIns(), twoSeats()};
    gateway.setPaused(true, 0);
    EXPECT_THROW(gateway.setSpeed(-1.0, 0), std::invalid_argument);
    Client alice{gateway, 0};
    Client bob{gateway, 0};
    alice.hello("alice", 0);
    bob.hello("bob", kSecond);
    ASSERT_TRUE(gateway.started());
    gateway.advance(3 * kSecond);
    EXPECT_EQ(gateway.now(), 0);

    gateway.setPaused(false, 3 * kSecond);
    gateway.advance(4 * kSecond);
    EXPECT_EQ(gateway.now(), kSecond);
    gateway.setSpeed(2.5, 4 * kSecond);
    EXPECT_EQ(gateway.speed(), 2.5);
    gateway.advance(6 * kSecond);
    EXPECT_EQ(gateway.now(), 6 * kSecond);
    gateway.setPaused(true, 6 * kSecond);
    EXPECT_TRUE(gateway.paused());
    gateway.advance(9 * kSecond);
    EXPECT_EQ(gateway.now(), 6 * kSecond);
    EXPECT_THROW(gateway.setSpeed(0.0, 9 * kSecond), std::invalid_argument);
}

// What a server waiting on many connections needs: the ones with something new to send, each
// once, and when to call advance next if nothing arrives.
TEST(GatewayTest, SaysWhichConnectionsAreReadyAndWhenToWake) {
    Gateway gateway{parseScenario(kScenario), std::string{kScenario},
                    AgentRegistry::withBuiltIns(), twoSeats()};
    Client alice{gateway, 0};
    Client bob{gateway, 0};
    // Before anyone says hello, only the hello deadline is due.
    ASSERT_TRUE(gateway.nextWake());
    EXPECT_EQ(*gateway.nextWake(), 5 * kSecond);
    alice.hello("alice", 0);
    std::vector<ConnectionId> ready;
    gateway.takeReady(ready);
    EXPECT_EQ(ready, (std::vector<ConnectionId>{alice.id()}));
    EXPECT_TRUE(gateway.urgent(alice.id())); // the welcome
    ready.clear();
    gateway.takeReady(ready);
    EXPECT_TRUE(ready.empty()); // each once
    alice.read();
    EXPECT_FALSE(gateway.urgent(alice.id()));

    bob.hello("bob", 0);
    // The agents start at once, so the market is due now; once it has run, its next event is due
    // later.
    EXPECT_EQ(gateway.nextWake(), 0);
    gateway.advance(0);
    alice.read();
    bob.read();
    gateway.takeReady(ready);
    // Running the market to any wall time before nextWake changes nothing a client could see;
    // running it to nextWake processes the market's next event.
    const std::int64_t next = *gateway.nextWake();
    EXPECT_GT(next, 0);
    EXPECT_LE(next, 100 * kMs); // a clock message at the latest
    const std::size_t pending = gateway.market().run.simulation().pendingCount();
    gateway.advance(next - 1);
    EXPECT_EQ(gateway.market().run.simulation().pendingCount(), pending);
    EXPECT_TRUE(alice.read().empty());

    // Market data alone is not urgent; an order's acknowledgement is.
    gateway.advance(2 * kSecond);
    ASSERT_FALSE(only<protocol::MarketMessage>(alice.read()).empty());
    gateway.advance(2 * kSecond + kMs);
    if (!gateway.pendingOutput(alice.id()).empty()) {
        EXPECT_FALSE(gateway.urgent(alice.id()));
    }
    alice.send(bid(1, 900, 1), 2 * kSecond + kMs);
    gateway.advance(2 * kSecond + 3 * kMs);
    EXPECT_TRUE(gateway.urgent(alice.id()));
}

// Each seat gets every trade with the moment it reached the seat, even when one trade is the
// same as the last but for its time, which the encoding shared between seats must not mix up.
TEST(GatewayTest, EveryTradeArrivesWithItsOwnTime) {
    test::RecordingSink exchange;
    Started market{twoSeats(), kScenario, &exchange};
    market.gateway.advance(5 * kSecond);
    std::vector<Timestamp> expected;
    for (const auto& [time, event] : exchange.events) {
        if (std::holds_alternative<Trade>(event)) {
            expected.push_back(time + kMs); // a millisecond to the seats
        }
    }
    for (Client* seat : {&market.alice, &market.bob}) {
        std::vector<Timestamp> received;
        for (const auto& message : only<protocol::MarketMessage>(seat->read())) {
            if (std::holds_alternative<Trade>(message.event)) {
                received.push_back(message.time);
            }
        }
        ASSERT_GT(expected.size(), 10U);
        // Trades still on their way at the end have not arrived.
        ASSERT_LE(received.size(), expected.size());
        EXPECT_EQ(received, std::vector<Timestamp>(expected.begin(),
                                                   expected.begin() + std::ssize(received)));
    }
}

// Conflating, a depth or top of book update still unsent when a newer one comes is replaced by
// it; everything else is kept, in time order.
TEST(GatewayTest, ConflatingSendsOnlyTheLatestSnapshots) {
    Started plain;
    Started conflated;
    conflated.gateway.conflate(true);
    for (Started* market : {&plain, &conflated}) {
        market->gateway.advance(3 * kSecond);
    }
    const auto everything = plain.alice.read();
    const auto latest = conflated.alice.read();
    ASSERT_LT(latest.size(), everything.size());
    // The same trades, and only the last depth and top of book.
    const auto trades = [](const std::vector<ServerMessage>& messages) {
        return std::ranges::count_if(only<protocol::MarketMessage>(messages), [](const auto& m) {
            return std::holds_alternative<Trade>(m.event);
        });
    };
    EXPECT_EQ(trades(latest), trades(everything));
    const auto last = [](const std::vector<ServerMessage>& messages, auto kind) {
        std::optional<protocol::MarketMessage> found;
        std::size_t count = 0;
        for (const auto& message : only<protocol::MarketMessage>(messages)) {
            if (std::holds_alternative<decltype(kind)>(message.event)) {
                found = message;
                ++count;
            }
        }
        return std::pair{found, count};
    };
    EXPECT_EQ(last(latest, BookDepth{}).first, last(everything, BookDepth{}).first);
    EXPECT_EQ(last(latest, BookDepth{}).second, 1U);
    EXPECT_EQ(last(latest, TopOfBook{}).first, last(everything, TopOfBook{}).first);
    EXPECT_EQ(last(latest, TopOfBook{}).second, 1U);
    Timestamp time = 0;
    for (const auto& message : only<protocol::MarketMessage>(latest)) {
        EXPECT_GE(message.time, time);
        time = message.time;
    }
}

TEST(GatewayTest, TheEndReportsTheSeatsResults) {
    Started market;
    market.alice.send(NewOrder{.clientOrderId = 1,
                               .side = Side::Buy,
                               .type = OrderType::Market,
                               .quantity = 3},
                      kSecond);
    market.gateway.advance(30 * kSecond);
    EXPECT_TRUE(market.gateway.finished());
    EXPECT_EQ(market.gateway.now(), 20 * kSecond);
    EXPECT_EQ(market.gateway.session().end, 20 * kSecond);
    const auto ends = only<protocol::End>(market.alice.read());
    ASSERT_EQ(ends.size(), 1U);
    const RunResult result = market.gateway.result();
    const auto alice = std::ranges::find(result.groups, "alice", &GroupResult::name);
    ASSERT_NE(alice, result.groups.end());
    EXPECT_EQ(ends[0].time, 20 * kSecond);
    EXPECT_EQ(ends[0].position, alice->position);
    EXPECT_EQ(ends[0].position, 3);
    EXPECT_EQ(ends[0].cash, alice->cash);
    EXPECT_EQ(ends[0].pnl, alice->pnl);
    EXPECT_TRUE(market.gateway.shouldClose(market.alice.id()));
    // Orders after the end go nowhere.
    market.alice.send(bid(2, 900, 1), 31 * kSecond);
    EXPECT_EQ(market.gateway.session().actions.size(), 1U);
}

// A scored challenge where taking liquidity is dear: a market buy of ten lots costs 50 in fees,
// past the loss limit of 30.
constexpr std::string_view kChallenge = R"(seed = 5
duration = "20s"
reference_price = 1000

[challenge]
name = "Do not take"
briefing = "Taking costs five a lot."

[scoring]
max_loss = 30
inventory_penalty = 0.01

[exchange]
taker_fee = 5.0

[participant]
latency = { to_exchange = "1ms", from_exchange = "1ms" }
account = { max_position = 50, max_order_quantity = 10 }

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

TEST(GatewayTest, TheWelcomeCarriesTheChallengeAndItsScoring) {
    Gateway gateway{parseScenario(kChallenge), std::string{kChallenge},
                    AgentRegistry::withBuiltIns(), {}};
    Client client{gateway, 0};
    client.hello("you", 0);
    const auto welcome = only<protocol::Welcome>(client.read());
    ASSERT_EQ(welcome.size(), 1U);
    ASSERT_TRUE(welcome[0].challenge);
    EXPECT_EQ(welcome[0].challenge->name, "Do not take");
    ASSERT_TRUE(welcome[0].scoring);
    EXPECT_EQ(welcome[0].scoring->maxLoss, 30);
    EXPECT_EQ(welcome[0].scoring->inventoryPenalty, 10);
}

TEST(GatewayTest, TheLossLimitStopsASeatAndTheReplayAgrees) {
    std::ostringstream live;
    CsvEventLog sink{live};
    Gateway gateway{parseScenario(kChallenge), std::string{kChallenge},
                    AgentRegistry::withBuiltIns(), {}, &sink};
    Client client{gateway, 0};
    client.hello("you", 0);
    gateway.advance(kSecond);
    client.read();
    client.send(bid(1, 900, 2), kSecond);
    client.send(NewOrder{.clientOrderId = 2,
                         .side = Side::Buy,
                         .type = OrderType::Market,
                         .quantity = 10},
                kSecond);
    gateway.advance(kSecond + 10 * kMs);
    const auto stopped = errors(client.read());
    ASSERT_EQ(stopped.size(), 1U);
    EXPECT_NE(stopped[0].message.find("stopped"), std::string::npos);
    // The resting bid was cancelled for the seat, and nothing new is taken.
    const auto& actions = gateway.session().actions;
    ASSERT_EQ(actions.size(), 3U);
    EXPECT_EQ(std::get<CancelOrder>(actions[2].request).clientOrderId,
              std::get<NewOrder>(actions[0].request).clientOrderId);
    client.send(bid(3, 900, 1), 2 * kSecond);
    const auto refused = events<OrderRejected>(client.read());
    ASSERT_EQ(refused.size(), 1U);
    EXPECT_EQ(refused[0].reason, RejectReason::LossLimit);
    EXPECT_EQ(gateway.session().actions.size(), 3U);

    gateway.advance(30 * kSecond);
    const auto ends = only<protocol::End>(client.read());
    ASSERT_EQ(ends.size(), 1U);
    ASSERT_TRUE(ends[0].score);
    ASSERT_TRUE(ends[0].score->stoppedAt);
    EXPECT_GT(ends[0].score->inventory, 0);

    std::ostringstream replayed;
    CsvEventLog replaySink{replayed};
    const RunResult result =
        replaySession(gateway.session(), AgentRegistry::withBuiltIns(), &replaySink);
    EXPECT_EQ(replayed.str(), live.str());
    ASSERT_EQ(result.scores.size(), 1U);
    EXPECT_EQ(result.scores[0].score, *ends[0].score);
}

// The rows of a CSV event log up to and including `time`.
std::string rowsUntil(const std::string& log, Timestamp time) {
    std::istringstream lines{log};
    std::string kept;
    std::string line;
    std::getline(lines, line);
    kept += line + "\n"; // the header
    while (std::getline(lines, line)) {
        if (std::stoll(line.substr(0, line.find(','))) > time) {
            break;
        }
        kept += line + "\n";
    }
    return kept;
}

// A served session rewound to a moment is the same market up to there; its seats are handed back
// with their open orders, under the ids those orders have inside the market, and the new session
// replays exactly.
TEST(GatewayTest, ARewoundSessionServesOnFromTheMoment) {
    std::ostringstream first;
    CsvEventLog firstSink{first};
    Started original{twoSeats(), kScenario, &firstSink};
    original.alice.send(bid(1, 900, 2), kSecond);
    original.bob.send(offer(1, 1'100, 3), kSecond);
    original.alice.send(bid(2, 905, 1), 2 * kSecond);
    original.gateway.advance(3 * kSecond);
    original.alice.send(protocol::ClientMessage{CancelOrder{.clientOrderId = 2}}, 3 * kSecond);
    original.gateway.stop(5 * kSecond);
    const Session session = original.gateway.session();
    ASSERT_EQ(session.actions.size(), 4U);

    std::ostringstream second;
    CsvEventLog secondSink{second};
    GatewayOptions options;
    options.rewind = session;
    options.rewindAt = 2'500 * kMs;
    Gateway rewound{parseScenario(kScenario), "", AgentRegistry::withBuiltIns(), options,
                    &secondSink};
    EXPECT_EQ(rewound.now(), 2'500 * kMs);
    EXPECT_EQ(rewound.session().actions.size(), 3U); // the cancel at 3s is cut
    EXPECT_EQ(rewound.session().seats, session.seats);
    EXPECT_EQ(rowsUntil(second.str(), 2'500 * kMs), rowsUntil(first.str(), 2'500 * kMs));

    Client alice{rewound, 0};
    alice.hello("alice", 0);
    const auto welcome = only<protocol::Welcome>(alice.read());
    ASSERT_EQ(welcome.size(), 1U);
    ASSERT_EQ(welcome[0].orders.size(), 2U);
    EXPECT_EQ(welcome[0].time, 2'500 * kMs);
    const ClientOrderId kept = welcome[0].orders[1].clientOrderId;
    Client bob{rewound, 0};
    bob.hello("bob", 0);
    EXPECT_TRUE(rewound.started());
    // Alice cancels the other order this time, by the id her welcome gave it.
    alice.send(protocol::ClientMessage{CancelOrder{.clientOrderId = welcome[0].orders[0]
                                                                      .clientOrderId}},
               100 * kMs);
    rewound.advance(200 * kMs);
    const auto cancelled = events<OrderCancelled>(alice.read());
    ASSERT_EQ(cancelled.size(), 1U);
    EXPECT_EQ(cancelled[0].clientOrderId, welcome[0].orders[0].clientOrderId);
    alice.send(bid(kept, 1, 1), 300 * kMs); // a live id is still taken
    EXPECT_EQ(events<OrderRejected>(alice.read()).size(), 1U);
    rewound.stop(kSecond);

    std::ostringstream replayed;
    CsvEventLog replaySink{replayed};
    static_cast<void>(
        replaySession(rewound.session(), AgentRegistry::withBuiltIns(), &replaySink));
    EXPECT_EQ(replayed.str(), second.str());
}

// A prediction market's clients learn that its prices stop at 99.
TEST(GatewayTest, TheWelcomeCarriesTheHighestPrice) {
    constexpr std::string_view kQuestion = R"(duration = "20s"
[prediction]
probability = 0.3
[[agents]]
type = "zero_intelligence"
)";
    Gateway gateway{parseScenario(kQuestion), std::string{kQuestion},
                    AgentRegistry::withBuiltIns(), {}};
    Client client{gateway, 0};
    client.hello("you", 0);
    const auto welcome = std::get<protocol::Welcome>(client.read().at(0));
    EXPECT_EQ(welcome.maxPrice, 99);
    EXPECT_EQ(welcome.referencePrice, 30);
}

TEST(GatewayTest, OptionsAreChecked) {
    const Scenario scenario = parseScenario(kScenario);
    const AgentRegistry registry = AgentRegistry::withBuiltIns();
    const auto build = [&](GatewayOptions options) {
        Gateway gateway{scenario, std::string{kScenario}, registry, std::move(options)};
    };
    GatewayOptions badSpeed;
    badSpeed.speed = 0.0;
    EXPECT_THROW(build(badSpeed), std::invalid_argument);
    GatewayOptions badSeat;
    badSeat.seats = {"a b"};
    EXPECT_THROW(build(badSeat), std::invalid_argument);
    GatewayOptions missingToken = twoSeats();
    missingToken.tokens = {{"alice", "red"}};
    EXPECT_THROW(build(missingToken), std::invalid_argument);
    GatewayOptions extraToken;
    extraToken.tokens = {{"you", "red"}, {"them", "blue"}};
    EXPECT_THROW(build(extraToken), std::invalid_argument);
    GatewayOptions badRate;
    badRate.maxMessagesPerSecond = 0;
    EXPECT_THROW(build(badRate), std::invalid_argument);
    GatewayOptions groupName;
    groupName.seats = {"maker"};
    EXPECT_THROW(build(groupName), ScenarioError);
    GatewayOptions pastTheEnd;
    pastTheEnd.rewind = Session{.end = kSecond};
    pastTheEnd.rewindAt = 2 * kSecond;
    EXPECT_THROW(build(pastTheEnd), std::invalid_argument);
}

} // namespace
} // namespace crowdbook

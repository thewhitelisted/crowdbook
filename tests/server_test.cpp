#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "crowdbook/protocol.hpp"
#include "crowdbook/scenario_file.hpp"
#include "crowdbook/server.hpp"

namespace crowdbook {
namespace {

constexpr std::string_view kScenario = R"(duration = "60s"
reference_price = 1000

[participant]
latency = { to_exchange = "1ms", from_exchange = "1ms" }

[[agents]]
type = "zero_intelligence"
count = 5
)";

// A blocking client socket that reads one line at a time, giving up after a few seconds.
class LineClient {
public:
    explicit LineClient(std::uint16_t port) : fd_(::socket(AF_INET, SOCK_STREAM, 0)) {
        timeval timeout{.tv_sec = 5, .tv_usec = 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
#ifdef SO_NOSIGPIPE
        const int on = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connected_ =
            ::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
    }
    LineClient(const LineClient&) = delete;
    LineClient& operator=(const LineClient&) = delete;
    ~LineClient() { ::close(fd_); }

    [[nodiscard]] bool connected() const noexcept { return connected_; }

    void send(const protocol::ClientMessage& message) const {
        const std::string line = protocol::encode(message);
        ASSERT_EQ(::send(fd_, line.data(), line.size(), 0), static_cast<ssize_t>(line.size()));
    }

    // Sends without checking that it went, for a server that may have stopped listening.
    void sendQuietly(const protocol::ClientMessage& message) const {
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const std::string line = protocol::encode(message);
        static_cast<void>(::send(fd_, line.data(), line.size(), flags));
    }

    // The next message, or nullopt if the connection closed or nothing came in time.
    std::optional<protocol::ServerMessage> read() {
        while (true) {
            if (const std::size_t newline = buffered_.find('\n'); newline != std::string::npos) {
                const std::string line = buffered_.substr(0, newline + 1);
                buffered_.erase(0, newline + 1);
                return protocol::decodeServer(line);
            }
            char chunk[4096];
            const ssize_t received = ::recv(fd_, chunk, sizeof chunk, 0);
            if (received <= 0) {
                return std::nullopt;
            }
            buffered_.append(chunk, static_cast<std::size_t>(received));
        }
    }

    // Reads until a message of type T arrives, which it returns, or the connection ends.
    template <typename T>
    std::optional<T> readUntil() {
        while (const auto message = read()) {
            if (const auto* found = std::get_if<T>(&*message)) {
                return *found;
            }
        }
        return std::nullopt;
    }

private:
    int fd_;
    bool connected_ = false;
    std::string buffered_;
};

TEST(ServerTest, AClientTradesOverARealSocket) {
    const Scenario scenario = parseScenario(kScenario);
    Gateway gateway{scenario, std::string{kScenario}, AgentRegistry::withBuiltIns(), {}};
    std::atomic<bool> stop{false};
    std::promise<std::uint16_t> listening;
    std::thread server{[&] {
        serve(gateway, {.host = "127.0.0.1", .port = 0}, stop,
              [&](std::uint16_t port) { listening.set_value(port); });
    }};
    const std::uint16_t port = listening.get_future().get();
    ASSERT_NE(port, 0);
    {
        LineClient client{port};
        ASSERT_TRUE(client.connected());
        client.send(protocol::Hello{.seat = "you"});
        ASSERT_TRUE(client.readUntil<protocol::Welcome>());
        ASSERT_TRUE(client.readUntil<protocol::Start>());
        client.send(NewOrder{.clientOrderId = 12,
                             .side = Side::Buy,
                             .type = OrderType::Limit,
                             .price = 900,
                             .quantity = 1});
        std::optional<OrderAccepted> accepted;
        while (!accepted) {
            const auto message = client.readUntil<protocol::MarketMessage>();
            ASSERT_TRUE(message);
            if (const auto* event = std::get_if<OrderAccepted>(&message->event)) {
                accepted = *event;
            }
        }
        EXPECT_EQ(accepted->clientOrderId, 12U);
        stop = true;
        const auto end = client.readUntil<protocol::End>();
        ASSERT_TRUE(end);
        EXPECT_FALSE(client.read()); // and the server closes the connection
    }
    server.join();
    EXPECT_TRUE(gateway.finished());
    EXPECT_EQ(gateway.session().actions.size(), 1U);
}

// Market data held back for a slow feed does not hold back the seat's own order events, and
// what arrives is in time order. Spinning before deadlines changes nothing a client can see.
TEST(ServerTest, BatchedMarketDataLeavesOrderEventsOnTime) {
    const Scenario scenario = parseScenario(kScenario);
    Gateway gateway{scenario, std::string{kScenario}, AgentRegistry::withBuiltIns(), {}};
    std::atomic<bool> stop{false};
    std::promise<std::uint16_t> listening;
    std::thread server{[&] {
        serve(gateway,
              {.host = "127.0.0.1", .port = 0, .spin = 50 * kMicrosecond,
               .feedInterval = 300 * kMillisecond},
              stop, [&](std::uint16_t port) { listening.set_value(port); });
    }};
    const std::uint16_t port = listening.get_future().get();
    {
        LineClient client{port};
        ASSERT_TRUE(client.connected());
        client.send(protocol::Hello{.seat = "you"});
        ASSERT_TRUE(client.readUntil<protocol::Start>());
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        const auto sent = std::chrono::steady_clock::now();
        client.send(NewOrder{.clientOrderId = 3, .side = Side::Buy, .price = 900, .quantity = 1});
        Timestamp time = 0;
        bool accepted = false;
        while (!accepted) {
            const auto message = client.readUntil<protocol::MarketMessage>();
            ASSERT_TRUE(message);
            EXPECT_GE(message->time, time);
            time = message->time;
            accepted = std::holds_alternative<OrderAccepted>(message->event);
        }
        // Sooner than the next feed tick: the answer went at once.
        EXPECT_LT(std::chrono::steady_clock::now() - sent, std::chrono::milliseconds{200});
        // And the market data held back arrives on the ticks.
        std::size_t marketData = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        while (std::chrono::steady_clock::now() < until) {
            const auto message = client.readUntil<protocol::MarketMessage>();
            ASSERT_TRUE(message);
            EXPECT_GE(message->time, time);
            time = message->time;
            marketData += recipient(message->event) ? 0U : 1U;
        }
        EXPECT_GT(marketData, 0U);
        stop = true;
        ASSERT_TRUE(client.readUntil<protocol::End>());
    }
    server.join();
}

// The server sleeps until the market's next event is due, not a moment longer: an order's answer,
// due two milliseconds after it is sent, arrives close to then.
TEST(ServerTest, AnswersLeaveWhenTheMarketMakesThem) {
    const Scenario scenario = parseScenario(kScenario);
    Gateway gateway{scenario, std::string{kScenario}, AgentRegistry::withBuiltIns(), {}};
    std::atomic<bool> stop{false};
    std::promise<std::uint16_t> listening;
    std::thread server{[&] {
        serve(gateway, {.host = "127.0.0.1", .port = 0}, stop,
              [&](std::uint16_t port) { listening.set_value(port); });
    }};
    const std::uint16_t port = listening.get_future().get();
    {
        LineClient client{port};
        client.send(protocol::Hello{.seat = "you"});
        ASSERT_TRUE(client.readUntil<protocol::Start>());
        std::vector<std::chrono::steady_clock::duration> trips;
        for (ClientOrderId id = 1; id <= 21; ++id) {
            const auto sent = std::chrono::steady_clock::now();
            client.send(
                NewOrder{.clientOrderId = id, .side = Side::Buy, .price = 900, .quantity = 1});
            bool accepted = false;
            while (!accepted) {
                const auto message = client.readUntil<protocol::MarketMessage>();
                ASSERT_TRUE(message);
                const auto* answer = std::get_if<OrderAccepted>(&message->event);
                accepted = answer != nullptr && answer->clientOrderId == id;
            }
            trips.push_back(std::chrono::steady_clock::now() - sent);
        }
        std::ranges::sort(trips);
        // Two milliseconds of the market's own latency, and little more: a server that slept
        // past the answer would take tens.
        EXPECT_LT(trips[trips.size() / 2], std::chrono::milliseconds{15});
        stop = true;
        ASSERT_TRUE(client.readUntil<protocol::End>());
    }
    server.join();
}

// A client that stops reading fills its socket, so sends block; once it reads again, everything
// waiting reaches it, the end included, though nothing new is added after the end.
TEST(ServerTest, AClientThatFallsBehindStillGetsEverything) {
    constexpr std::string_view kBusy = R"(duration = "60s"
reference_price = 1000

[exchange]
depth_levels = 20

[[agents]]
type = "zero_intelligence"
count = 40
limit_rate = 20.0
market_rate = 2.0
cancel_rate = 10.0
)";
    const Scenario scenario = parseScenario(kBusy);
    GatewayOptions options;
    options.speed = 10.0;
    options.maxPendingOutput = std::size_t{256} << 20;
    Gateway gateway{scenario, std::string{kBusy}, AgentRegistry::withBuiltIns(), options};
    std::atomic<bool> stop{false};
    std::promise<std::uint16_t> listening;
    std::thread server{[&] {
        serve(gateway, {.host = "127.0.0.1", .port = 0, .closeGrace = 20 * kSecond}, stop,
              [&](std::uint16_t port) { listening.set_value(port); });
    }};
    const std::uint16_t port = listening.get_future().get();
    {
        LineClient client{port};
        client.send(protocol::Hello{.seat = "you"});
        ASSERT_TRUE(client.readUntil<protocol::Start>());
        std::this_thread::sleep_for(std::chrono::seconds{1}); // megabytes of market data pile up
        stop = true;
        ASSERT_TRUE(client.readUntil<protocol::End>());
    }
    server.join();
}

TEST(ServerTest, AClientThatHangsUpHasItsOrdersCancelled) {
    const Scenario scenario = parseScenario(kScenario);
    Gateway gateway{scenario, std::string{kScenario}, AgentRegistry::withBuiltIns(), {}};
    std::atomic<bool> stop{false};
    std::promise<std::uint16_t> listening;
    std::thread server{[&] {
        serve(gateway, {.host = "127.0.0.1", .port = 0}, stop,
              [&](std::uint16_t port) { listening.set_value(port); });
    }};
    const std::uint16_t port = listening.get_future().get();
    {
        LineClient client{port};
        client.send(protocol::Hello{.seat = "you"});
        ASSERT_TRUE(client.readUntil<protocol::Start>());
        client.send(NewOrder{.clientOrderId = 1,
                             .side = Side::Buy,
                             .type = OrderType::Limit,
                             .price = 900,
                             .quantity = 1});
        ASSERT_TRUE(client.readUntil<protocol::MarketMessage>());
    }
    // The server notices the hang-up within a poll or two and cancels the order; a second is
    // plenty. The recording, read once the server has stopped, shows the cancel.
    std::this_thread::sleep_for(std::chrono::seconds{1});
    stop = true;
    server.join();
    ASSERT_EQ(gateway.session().actions.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<CancelOrder>(gateway.session().actions[1].request));
}

// Two clients trade over real sockets, one leaving early; the recording replays the session's
// event log byte for byte, whatever the timing of the network was.
TEST(ServerTest, ASessionOverSocketsReplaysByteForByte) {
    const Scenario scenario = parseScenario(kScenario);
    std::ostringstream live;
    CsvEventLog sink{live};
    GatewayOptions options;
    options.seats = {"alice", "bob"};
    Gateway gateway{scenario, std::string{kScenario}, AgentRegistry::withBuiltIns(), options,
                    &sink};
    std::atomic<bool> stop{false};
    std::promise<std::uint16_t> listening;
    std::thread server{[&] {
        serve(gateway, {.host = "127.0.0.1", .port = 0}, stop,
              [&](std::uint16_t port) { listening.set_value(port); });
    }};
    const std::uint16_t port = listening.get_future().get();
    {
        LineClient alice{port};
        LineClient bob{port};
        alice.send(protocol::Hello{.seat = "alice"});
        bob.send(protocol::Hello{.seat = "bob"});
        ASSERT_TRUE(alice.readUntil<protocol::Start>());
        ASSERT_TRUE(bob.readUntil<protocol::Start>());
        for (ClientOrderId id = 1; id <= 5; ++id) {
            alice.send(NewOrder{.clientOrderId = id,
                                .side = Side::Buy,
                                .type = OrderType::Limit,
                                .price = 1'000 - static_cast<Price>(id),
                                .quantity = 1});
            bob.send(NewOrder{.clientOrderId = id,
                              .side = Side::Sell,
                              .type = OrderType::Market,
                              .quantity = 1});
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    } // both hang up, and their open orders are cancelled
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    stop = true;
    server.join();
    EXPECT_GE(gateway.session().actions.size(), 10U);

    std::ostringstream replayed;
    CsvEventLog replaySink{replayed};
    static_cast<void>(replaySession(gateway.session(), AgentRegistry::withBuiltIns(), &replaySink));
    EXPECT_EQ(replayed.str(), live.str());
}

// A client still sending when the session ends gets its last messages all the same: the server
// stops writing but goes on reading, so the client's unsent orders never reset the connection.
TEST(ServerTest, AClientStillSendingWhenTheSessionEndsGetsTheEnd) {
    const Scenario scenario = parseScenario(kScenario);
    Gateway gateway{scenario, std::string{kScenario}, AgentRegistry::withBuiltIns(), {}};
    std::atomic<bool> stop{false};
    std::promise<std::uint16_t> listening;
    std::thread server{[&] {
        serve(gateway, {.host = "127.0.0.1", .port = 0}, stop,
              [&](std::uint16_t port) { listening.set_value(port); });
    }};
    const std::uint16_t port = listening.get_future().get();
    {
        LineClient client{port};
        client.send(protocol::Hello{.seat = "you"});
        ASSERT_TRUE(client.readUntil<protocol::Start>());
        stop = true;
        // Hundreds of kilobytes the server will not act on, sent without reading anything.
        for (ClientOrderId id = 1; id <= 3'000; ++id) {
            client.sendQuietly(NewOrder{.clientOrderId = id,
                                        .side = Side::Buy,
                                        .type = OrderType::Limit,
                                        .price = 900,
                                        .quantity = 1});
        }
        EXPECT_TRUE(client.readUntil<protocol::End>());
    }
    server.join();
}

TEST(ServerTest, ListeningOnABadAddressFails) {
    const Scenario scenario = parseScenario(kScenario);
    Gateway gateway{scenario, std::string{kScenario}, AgentRegistry::withBuiltIns(), {}};
    const std::atomic<bool> stop{false};
    EXPECT_THROW(serve(gateway, {.host = "not an address", .port = 0}, stop), NetworkError);
}

} // namespace
} // namespace crowdbook

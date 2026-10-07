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

TEST(ServerTest, ListeningOnABadAddressFails) {
    const Scenario scenario = parseScenario(kScenario);
    Gateway gateway{scenario, std::string{kScenario}, AgentRegistry::withBuiltIns(), {}};
    const std::atomic<bool> stop{false};
    EXPECT_THROW(serve(gateway, {.host = "not an address", .port = 0}, stop), NetworkError);
}

} // namespace
} // namespace crowdbook

#include <cstdint>
#include <exception>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/event_log.hpp"
#include "crowdbook/gateway.hpp"
#include "crowdbook/protocol.hpp"
#include "crowdbook/random.hpp"
#include "crowdbook/scenario_file.hpp"

// Hostile clients against a gateway: connections that claim seats rightly and wrongly, send
// orders with ids and prices anywhere, mangled lines, random bytes, endless lines and floods, read
// slowly or never, and come and go, while the market runs and its clock changes speed. Whatever
// they do, the gateway must not throw, everything it sends must be whole protocol messages, no
// connection may hold more output than its limit, the book must stay consistent, and the session
// must replay to the same event log. Part of the main tests, so the sanitizers run it too.

namespace crowdbook {
namespace {

constexpr std::string_view kScenario = R"(seed = 9
duration = "6s"
reference_price = 1000

[exchange]
depth_levels = 5

[participant]
latency = { to_exchange = "1ms", from_exchange = "1ms" }
account = { max_position = 40, max_order_quantity = 10 }

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

constexpr std::int64_t kMs = kMillisecond;

const std::vector<std::string> kSeats = {"ana", "bo", "cy"};

// A value that is sometimes ordinary, sometimes at or past an edge.
std::int64_t anyNumber(Random& random, std::int64_t low, std::int64_t high) {
    switch (random.below(8)) {
    case 0:
        return std::numeric_limits<std::int64_t>::max();
    case 1:
        return std::numeric_limits<std::int64_t>::min();
    case 2:
        return 0;
    case 3:
        return -random.uniformInt(1, 1'000);
    default:
        return random.uniformInt(low, high);
    }
}

std::string anyRequest(Random& random) {
    // Mostly ids a seat might really use, so cancels and modifies find their orders.
    const auto id = random.below(10) == 0
                        ? static_cast<ClientOrderId>(anyNumber(random, 0, 1'000'000))
                        : static_cast<ClientOrderId>(random.uniformInt(1, 30));
    switch (random.below(4)) {
    case 0:
        return protocol::encode(protocol::ClientMessage{CancelOrder{.clientOrderId = id}});
    case 1:
        return protocol::encode(protocol::ClientMessage{
            ModifyOrder{.clientOrderId = id,
                        .price = anyNumber(random, 950, 1'050),
                        .quantity = anyNumber(random, 1, 12)}});
    default:
        return protocol::encode(protocol::ClientMessage{NewOrder{
            .clientOrderId = id,
            .side = random.below(2) == 0 ? Side::Buy : Side::Sell,
            .type = random.below(5) == 0 ? OrderType::Market : OrderType::Limit,
            .timeInForce = static_cast<TimeInForce>(random.below(3)),
            .price = anyNumber(random, 950, 1'050),
            .quantity = anyNumber(random, 1, 12),
            .parent = random.below(2)}});
    }
}

std::string anyHello(Random& random) {
    const std::size_t seat = random.below(kSeats.size() + 1);
    protocol::Hello hello{.seat = seat < kSeats.size() ? kSeats[seat] : "nobody"};
    switch (random.below(4)) {
    case 0:
        break; // no token
    case 1:
        hello.token = "wrong";
        break;
    default:
        hello.token = "token-" + hello.seat;
    }
    if (random.below(10) == 0) {
        hello.protocol = anyNumber(random, 0, 3);
    }
    return protocol::encode(protocol::ClientMessage{hello});
}

// Changes a few bytes of a line: replaces, deletes, inserts, cuts it short or repeats a piece.
std::string mangle(Random& random, std::string line) {
    const auto changes = random.uniformInt(1, 4);
    for (std::int64_t change = 0; change < changes && !line.empty(); ++change) {
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
            const auto length = static_cast<std::size_t>(random.below(line.size() - at)) + 1;
            line.insert(at, line.substr(at, length));
            break;
        }
        }
    }
    return line;
}

// What a hostile client might send next.
std::string anyBytes(Random& random) {
    switch (random.below(10)) {
    case 0:
        return anyHello(random);
    case 1:
    case 2:
    case 3:
        return anyRequest(random);
    case 4:
        return mangle(random, random.below(3) == 0 ? anyHello(random) : anyRequest(random));
    case 5: {
        std::string noise(static_cast<std::size_t>(random.below(120)), '\0');
        for (char& byte : noise) {
            byte = static_cast<char>(random.below(256));
        }
        return noise;
    }
    case 6:
        // A line far longer than any message, which may or may not end.
        return std::string(static_cast<std::size_t>(random.uniformInt(1'000, 80'000)), 'x') +
               (random.below(2) == 0 ? "\n" : "");
    case 7: {
        std::string burst;
        for (std::int64_t i = random.uniformInt(20, 120); i > 0; --i) {
            burst += anyRequest(random);
        }
        return burst;
    }
    case 8:
        return random.below(2) == 0 ? "\n" : "\r\n";
    default:
        return anyRequest(random) + anyRequest(random);
    }
}

// What the hostile sessions got up to, so that the test can check it reached the hard cases.
struct Coverage {
    std::size_t messages = 0;  // whole messages the gateway sent
    std::size_t errors = 0;    // of them, errors
    std::size_t fills = 0;     // seats' orders filled
    std::size_t closed = 0;    // connections the gateway told to close
    std::size_t actions = 0;   // requests that reached the market
};

struct Peer {
    ConnectionId id = 0;
    bool open = true;
    std::string received{}; // bytes taken from the gateway that do not yet make a whole line
};

void fuzzOnce(std::uint64_t seed, Coverage& coverage) {
    SCOPED_TRACE(testing::Message() << "seed " << seed);
    Random random{seed, 0};
    GatewayOptions options;
    options.seats = kSeats;
    for (const std::string& seat : kSeats) {
        options.tokens[seat] = "token-" + seat;
    }
    options.speed = 4.0;
    options.maxMessagesPerSecond = 100;
    options.maxPendingOutput = std::size_t{64} << 10;
    options.helloTimeout = 300 * kMs;
    std::ostringstream live;
    CsvEventLog liveLog{live};
    Gateway gateway{parseScenario(kScenario), std::string{kScenario},
                    AgentRegistry::withBuiltIns(), options, &liveLog};

    std::vector<Peer> peers;
    std::vector<ConnectionId> ready;
    std::int64_t wall = 0;
    int step = 0;
    // Takes some or all of what is waiting for a peer; every whole line must be a message.
    const auto read = [&](Peer& peer, bool everything) {
        const std::string_view pending = gateway.pendingOutput(peer.id);
        ASSERT_LE(pending.size(), options.maxPendingOutput);
        const std::size_t take = everything ? pending.size()
                                            : static_cast<std::size_t>(
                                                  random.below(pending.size() + 1));
        peer.received.append(pending.substr(0, take));
        gateway.consumeOutput(peer.id, take);
        std::size_t start = 0;
        for (std::size_t end = peer.received.find('\n'); end != std::string::npos;
             end = peer.received.find('\n', start)) {
            const std::string_view line{peer.received.data() + start, end - start + 1};
            try {
                const protocol::ServerMessage message = protocol::decodeServer(line);
                ++coverage.messages;
                if (std::holds_alternative<protocol::Error>(message)) {
                    ++coverage.errors;
                } else if (const auto* market = std::get_if<protocol::MarketMessage>(&message)) {
                    if (std::holds_alternative<OrderFilled>(market->event)) {
                        ++coverage.fills;
                    }
                }
            } catch (const protocol::ProtocolError& error) {
                ADD_FAILURE() << "step " << step << ": " << error.what() << " in " << line;
            }
            start = end + 1;
        }
        peer.received.erase(0, start);
    };
    const auto anyOpen = [&]() -> Peer* {
        std::vector<Peer*> open;
        for (Peer& peer : peers) {
            if (peer.open) {
                open.push_back(&peer);
            }
        }
        return open.empty() ? nullptr : open[random.below(open.size())];
    };

    try {
        // The seats are claimed rightly first, so that the market runs; anyone may take them over
        // once they are free.
        for (const std::string& seat : kSeats) {
            peers.push_back(Peer{.id = gateway.connect(wall)});
            gateway.receive(peers.back().id,
                            protocol::encode(protocol::ClientMessage{
                                protocol::Hello{.seat = seat, .token = "token-" + seat}}),
                            wall);
        }
        for (; step < 4'000 && !gateway.finished() && !testing::Test::HasFatalFailure(); ++step) {
            wall += random.below(4) == 0 ? 0 : random.uniformInt(1, 3 * kMs);
            const double what = random.uniform();
            Peer* peer = anyOpen();
            if (peer == nullptr || (what < 0.04 && peers.size() < 40)) {
                peers.push_back(Peer{.id = gateway.connect(wall)});
            } else if (what < 0.07) {
                gateway.disconnect(peer->id, wall);
                peer->open = false;
            } else if (what < 0.75) {
                // Sent in pieces, as a network delivers it.
                const std::string bytes = anyBytes(random);
                std::size_t start = 0;
                while (start < bytes.size()) {
                    const auto piece =
                        static_cast<std::size_t>(random.uniformInt(1, 4'096));
                    gateway.receive(peer->id, std::string_view{bytes}.substr(start, piece), wall);
                    start += piece;
                }
            } else if (what < 0.995) {
                read(*peer, random.below(3) == 0);
            } else if (random.below(2) == 0) {
                gateway.setSpeed(0.5 + 8.0 * random.uniform(), wall);
            } else {
                gateway.setPaused(!gateway.paused(), wall);
            }
            gateway.advance(wall);
            ready.clear();
            gateway.takeReady(ready);
            static_cast<void>(gateway.nextWake());
            for (Peer& each : peers) {
                if (each.open) {
                    static_cast<void>(gateway.urgent(each.id));
                    ASSERT_LE(gateway.pendingOutput(each.id).size(), options.maxPendingOutput);
                    if (gateway.shouldClose(each.id)) {
                        ++coverage.closed;
                        read(each, true);
                        gateway.disconnect(each.id, wall);
                        each.open = false;
                    }
                }
            }
        }
        gateway.stop(wall);
        for (Peer& peer : peers) {
            if (peer.open) {
                read(peer, true);
            }
        }
    } catch (const std::exception& error) {
        FAIL() << "step " << step << ": the gateway threw " << error.what();
    }

    EXPECT_TRUE(gateway.finished());
    coverage.actions += gateway.session().actions.size();
    const std::optional<std::string> problem =
        gateway.market().run.simulation().exchange().book().audit();
    EXPECT_FALSE(problem) << *problem;
    std::ostringstream replayed;
    CsvEventLog replayLog{replayed};
    static_cast<void>(replaySession(gateway.session(), AgentRegistry::withBuiltIns(), &replayLog));
    EXPECT_EQ(replayed.str(), live.str()) << "the session does not replay to its live event log";
}

TEST(GatewayFuzzTest, HostileClientsCannotBreakTheMarket) {
    Coverage coverage;
    for (std::uint64_t seed = 1; seed <= 12 && !HasFatalFailure(); ++seed) {
        fuzzOnce(seed, coverage);
    }
    // The sessions reached the hard cases: clients that traded, errors, closed connections.
    EXPECT_GT(coverage.messages, 5'000U);
    EXPECT_GT(coverage.errors, 500U);
    EXPECT_GT(coverage.fills, 50U);
    EXPECT_GT(coverage.closed, 200U);
    EXPECT_GT(coverage.actions, 1'000U);
}

} // namespace
} // namespace crowdbook

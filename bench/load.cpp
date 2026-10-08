// Load test for the server: many clients trading over TCP, timing each order's round trip.
//
//   crowdbook_load [--clients N] [--threads T] [--rate R] [--seconds S] [--scenario FILE]
//                  [--server-spin MICROSECONDS] [--feed-interval MICROSECONDS]
//                  [--client-spin yes] [--connect HOST:PORT]
//
// Each client claims a seat, then sends R requests a second, at random moments: a resting bid far
// below the market, then its cancel, and so on. It times each from the moment it is sent to the
// moment its answer (accepted, cancelled or rejected) arrives, while it reads every market data
// message too. Each answer is due after the participant's latency to the exchange and back, by
// the market's clock; how much later than that it arrives is what the server adds.
//
// Without --connect it serves the scenario itself, on a thread of its own, and reports the CPU
// time that thread used. With --connect it loads a server started separately, with seats named
// bot1 to botN:
//
//   crowdbook serve examples/scenarios/playable.toml --listen :7878 --seat bot1 --seat bot2 ...

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <format>
#include <future>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/gateway.hpp"
#include "crowdbook/protocol.hpp"
#include "crowdbook/random.hpp"
#include "crowdbook/scenario_file.hpp"
#include "crowdbook/server.hpp"

namespace crowdbook {
namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

struct Options {
    std::size_t clients = 50;
    std::size_t threads = 4; // client threads
    double rate = 20.0; // requests a second per client
    double seconds = 10.0;
    std::string scenario = std::string{CROWDBOOK_SOURCE_DIR} + "/examples/scenarios/playable.toml";
    std::optional<std::string> host{};
    std::uint16_t port = 0;
    // The server's spin, and whether the clients spin too rather than sleep, so that their own
    // wakeups add nothing to the round trips.
    std::int64_t serverSpin = 0;
    bool clientSpin = false;
    std::int64_t feedInterval = 0;
};

std::int64_t wallNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::int64_t threadCpuNow() {
    timespec time{};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time);
    return std::int64_t{time.tv_sec} * kSecond + time.tv_nsec;
}

int connectTo(const std::string& host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0) {
        throw std::runtime_error(std::format("cannot resolve {}", host));
    }
    int fd = -1;
    for (addrinfo* address = found; address != nullptr && fd < 0; address = address->ai_next) {
        fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd >= 0 && ::connect(fd, address->ai_addr, address->ai_addrlen) < 0) {
            ::close(fd);
            fd = -1;
        }
    }
    ::freeaddrinfo(found);
    if (fd < 0) {
        throw std::runtime_error(std::format("cannot connect to {}:{}: {}", host, port,
                                             std::strerror(errno)));
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    const int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    return fd;
}

struct Client {
    int fd = -1;
    std::string input{};
    std::string output{};
    bool started = false;
    bool ended = false;
    ClientOrderId nextId = 1;
    std::optional<ClientOrderId> resting{};  // the bid to cancel next
    std::optional<ClientOrderId> awaiting{}; // the request whose answer is due
    std::int64_t sentAt = 0;
    std::int64_t nextAt = 0;
    std::int64_t bytes = 0;
};

struct Results {
    std::vector<std::int64_t> roundTrips{};
    std::int64_t bytes = 0;
    std::int64_t rejected = 0;
    std::int64_t cpu = 0; // the client threads' CPU time
};

// Whether a line can be an answer, or the start or end: the market data that makes up most of
// what arrives is skipped without decoding it, so that the clients keep up with the server.
bool worthDecoding(std::string_view line) {
    constexpr std::string_view kPrefix = R"({"type":")";
    if (!line.starts_with(kPrefix)) {
        return true;
    }
    const std::string_view rest = line.substr(kPrefix.size());
    const std::string_view type = rest.substr(0, rest.find('"'));
    return type == "accepted" || type == "cancelled" || type == "rejected" || type == "start" ||
           type == "welcome" || type == "end" || type == "error";
}

// The answer to the request a client is waiting for, if this message is one.
void noteAnswer(Client& client, const protocol::ServerMessage& message, std::int64_t now,
                Results& results) {
    if (std::holds_alternative<protocol::Start>(message)) {
        client.started = true;
        return;
    }
    if (const auto* welcome = std::get_if<protocol::Welcome>(&message)) {
        client.started = welcome->started;
        return;
    }
    if (std::holds_alternative<protocol::End>(message)) {
        client.ended = true;
        return;
    }
    const auto* market = std::get_if<protocol::MarketMessage>(&message);
    if (market == nullptr || !client.awaiting) {
        return;
    }
    const ClientOrderId id = *client.awaiting;
    bool answered = false;
    if (const auto* accepted = std::get_if<OrderAccepted>(&market->event)) {
        answered = accepted->clientOrderId == id;
        if (answered) {
            client.resting = id;
        }
    } else if (const auto* cancelled = std::get_if<OrderCancelled>(&market->event)) {
        answered = cancelled->clientOrderId == id;
        if (answered) {
            client.resting.reset();
        }
    } else if (const auto* rejection = std::get_if<OrderRejected>(&market->event)) {
        answered = rejection->clientOrderId == id;
        if (answered) {
            ++results.rejected;
            if (rejection->request == RequestKind::Cancel) {
                client.resting.reset();
            }
        }
    }
    if (answered) {
        results.roundTrips.push_back(now - client.sentAt);
        client.awaiting.reset();
    }
}

// Runs the clients for seats first + 1 to first + count until the time is up. Returns what they
// measured.
Results runClients(const Options& options, const std::string& host, std::uint16_t port,
                   Price reference, std::size_t first, std::size_t count) {
    const std::int64_t cpuBefore = threadCpuNow();
    std::vector<Client> clients(count);
    for (std::size_t i = 0; i < clients.size(); ++i) {
        clients[i].fd = connectTo(host, port);
        clients[i].output =
            protocol::encode(protocol::Hello{.seat = std::format("bot{}", first + i + 1)});
    }
    Random random{7, first};
    const auto wait = [&] {
        return static_cast<std::int64_t>(random.exponential(options.rate) * kSecond);
    };
    Results results;
    std::vector<pollfd> polled(clients.size());
    std::optional<std::int64_t> startedAt;
    std::array<char, 64 * 1024> buffer{};
    while (true) {
        std::int64_t now = wallNow();
        const bool allStarted = std::ranges::all_of(clients, &Client::started);
        if (allStarted && !startedAt) {
            startedAt = now;
            for (Client& client : clients) {
                client.nextAt = now + wait();
            }
        }
        if (startedAt && now - *startedAt >= static_cast<std::int64_t>(options.seconds * kSecond)) {
            break;
        }
        if (std::ranges::any_of(clients, &Client::ended)) {
            throw std::runtime_error("the session ended before the test did: use a longer one");
        }
        std::int64_t nextAt = now + 10 * kMillisecond;
        for (Client& client : clients) {
            if (startedAt && !client.awaiting && now >= client.nextAt) {
                const ClientOrderId id = client.nextId++;
                if (client.resting) {
                    protocol::encodeTo(client.output,
                                       CancelOrder{.clientOrderId = *client.resting});
                    client.awaiting = *client.resting;
                } else {
                    protocol::encodeTo(client.output, NewOrder{.clientOrderId = id,
                                                               .side = Side::Buy,
                                                               .price = reference / 2,
                                                               .quantity = 1});
                    client.awaiting = id;
                }
                client.sentAt = now;
                client.nextAt = now + wait();
            }
            if (!client.awaiting) {
                nextAt = std::min(nextAt, client.nextAt);
            }
            if (!client.output.empty()) {
                const ssize_t sent =
                    ::send(client.fd, client.output.data(), client.output.size(), kSendFlags);
                if (sent > 0) {
                    client.output.erase(0, static_cast<std::size_t>(sent));
                }
            }
        }
        for (std::size_t i = 0; i < clients.size(); ++i) {
            polled[i] = {.fd = clients[i].fd,
                         .events = static_cast<short>(POLLIN |
                                                      (clients[i].output.empty() ? 0 : POLLOUT)),
                         .revents = 0};
        }
        const auto timeout =
            options.clientSpin
                ? 0
                : static_cast<int>(
                      std::clamp<std::int64_t>((nextAt - wallNow()) / kMillisecond, 0, 10));
        ::poll(polled.data(), static_cast<nfds_t>(polled.size()), timeout);
        now = wallNow();
        for (std::size_t i = 0; i < clients.size(); ++i) {
            if ((polled[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }
            Client& client = clients[i];
            const ssize_t received = ::recv(client.fd, buffer.data(), buffer.size(), 0);
            if (received <= 0) {
                if (received == 0 || (errno != EAGAIN && errno != EINTR)) {
                    throw std::runtime_error("the server closed a connection");
                }
                continue;
            }
            client.bytes += received;
            client.input.append(buffer.data(), static_cast<std::size_t>(received));
            std::size_t start = 0;
            for (std::size_t end = client.input.find('\n'); end != std::string::npos;
                 end = client.input.find('\n', start)) {
                const std::string_view line =
                    std::string_view{client.input}.substr(start, end - start + 1);
                if (worthDecoding(line)) {
                    noteAnswer(client, protocol::decodeServer(line), now, results);
                }
                start = end + 1;
            }
            client.input.erase(0, start);
        }
    }
    for (Client& client : clients) {
        results.bytes += client.bytes;
        ::close(client.fd);
    }
    results.cpu = threadCpuNow() - cpuBefore;
    return results;
}

// Runs the clients on several threads, each with its share of them, and puts their results
// together.
Results runAllClients(const Options& options, const std::string& host, std::uint16_t port,
                      Price reference) {
    const std::size_t threads = std::min(options.threads, options.clients);
    std::vector<std::future<Results>> parts;
    for (std::size_t t = 0; t < threads; ++t) {
        const std::size_t first = options.clients * t / threads;
        const std::size_t last = options.clients * (t + 1) / threads;
        parts.push_back(std::async(std::launch::async, [&, first, last] {
            return runClients(options, host, port, reference, first, last - first);
        }));
    }
    Results all;
    for (std::future<Results>& part : parts) {
        Results results = part.get();
        all.roundTrips.insert(all.roundTrips.end(), results.roundTrips.begin(),
                              results.roundTrips.end());
        all.bytes += results.bytes;
        all.rejected += results.rejected;
        all.cpu += results.cpu;
    }
    return all;
}

double percentile(std::span<const std::int64_t> sorted, double p) {
    const auto index = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
    return static_cast<double>(sorted[index]) / static_cast<double>(kMillisecond);
}

void report(std::string_view what, std::vector<std::int64_t> values) {
    std::ranges::sort(values);
    std::cout << std::format(
        "{:<38} p50 {:.3f}  p90 {:.3f}  p99 {:.3f}  p99.9 {:.3f}  max {:.3f} ms\n", what,
        percentile(values, 0.5), percentile(values, 0.9), percentile(values, 0.99),
        percentile(values, 0.999), percentile(values, 1.0));
}

Options parse(std::span<char*> args) {
    Options options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view flag = args[i];
        if (i + 1 >= args.size()) {
            throw std::invalid_argument(std::format("{} needs a value", flag));
        }
        const std::string_view value = args[++i];
        if (flag == "--clients") {
            options.clients = std::stoul(std::string{value});
        } else if (flag == "--threads") {
            options.threads = std::stoul(std::string{value});
        } else if (flag == "--rate") {
            options.rate = std::stod(std::string{value});
        } else if (flag == "--seconds") {
            options.seconds = std::stod(std::string{value});
        } else if (flag == "--server-spin") {
            options.serverSpin = std::stoll(std::string{value}) * kMicrosecond;
        } else if (flag == "--feed-interval") {
            options.feedInterval = std::stoll(std::string{value}) * kMicrosecond;
        } else if (flag == "--client-spin") {
            options.clientSpin = value == "yes";
        } else if (flag == "--scenario") {
            options.scenario = value;
        } else if (flag == "--connect") {
            const std::size_t colon = value.rfind(':');
            if (colon == std::string_view::npos) {
                throw std::invalid_argument("--connect needs host:port");
            }
            options.host = std::string{value.substr(0, colon)};
            options.port =
                static_cast<std::uint16_t>(std::stoul(std::string{value.substr(colon + 1)}));
        } else {
            throw std::invalid_argument(std::format("unknown option {}", flag));
        }
    }
    if (options.clients == 0 || options.threads == 0 || !(options.rate > 0.0) ||
        !(options.seconds > 0.0)) {
        throw std::invalid_argument("--clients, --threads, --rate and --seconds must be positive");
    }
    return options;
}

int run(std::span<char*> args) {
    const Options options = parse(args);
    const Scenario scenario = loadScenario(options.scenario);
    const Latency& latency = scenario.participant.latency;
    const std::int64_t marketRoundTrip = latency.toExchange + latency.fromExchange;

    std::atomic<bool> stop{false};
    std::int64_t serverCpu = 0;
    std::thread server;
    std::string host = options.host.value_or("127.0.0.1");
    std::uint16_t port = options.port;
    std::exception_ptr serverFailure;
    if (!options.host) {
        GatewayOptions gatewayOptions;
        gatewayOptions.seats.clear();
        for (std::size_t i = 1; i <= options.clients; ++i) {
            gatewayOptions.seats.push_back(std::format("bot{}", i));
        }
        std::promise<std::uint16_t> listening;
        std::future<std::uint16_t> bound = listening.get_future();
        server = std::thread([&, gatewayOptions] {
            try {
                Gateway gateway{scenario, "", AgentRegistry::withBuiltIns(), gatewayOptions};
                const std::int64_t before = threadCpuNow();
                crowdbook::serve(gateway,
                                 {.host = "127.0.0.1",
                                  .port = 0,
                                  .spin = options.serverSpin,
                                  .feedInterval = options.feedInterval},
                                 stop, [&](std::uint16_t p) { listening.set_value(p); });
                serverCpu = threadCpuNow() - before;
            } catch (...) {
                serverFailure = std::current_exception();
            }
        });
        port = bound.get();
    }

    const std::int64_t began = wallNow();
    Results results = runAllClients(options, host, port, scenario.referencePrice);
    const double elapsed = static_cast<double>(wallNow() - began) / kSecond;
    if (server.joinable()) {
        stop = true;
        server.join();
        if (serverFailure) {
            std::rethrow_exception(serverFailure);
        }
    }

    std::cout << std::format("{} clients, {:g} requests a second each, {:g} s; the market's own "
                             "round trip is {:.3f} ms\n",
                             options.clients, options.rate, options.seconds,
                             static_cast<double>(marketRoundTrip) / kMillisecond);
    if (results.roundTrips.empty()) {
        std::cout << "no round trips completed\n";
        return 1;
    }
    std::vector<std::int64_t> late;
    late.reserve(results.roundTrips.size());
    for (const std::int64_t trip : results.roundTrips) {
        late.push_back(trip - marketRoundTrip);
    }
    std::cout << std::format("{} round trips ({:.0f} a second), {} rejected; {:.1f} MB/s to the "
                             "clients\n",
                             results.roundTrips.size(),
                             static_cast<double>(results.roundTrips.size()) / options.seconds,
                             results.rejected, static_cast<double>(results.bytes) / 1e6 / elapsed);
    report("order to answer", results.roundTrips);
    report("beyond the market's own latency", late);
    if (!options.host) {
        std::cout << std::format("server thread: {:.3f} s of CPU in {:.1f} s ({:.1f}% of a core)\n",
                                 static_cast<double>(serverCpu) / kSecond, elapsed,
                                 100.0 * static_cast<double>(serverCpu) / kSecond / elapsed);
    }
    std::cout << std::format("client threads: {:.1f}% of a core each\n",
                             100.0 * static_cast<double>(results.cpu) / kSecond / elapsed /
                                 static_cast<double>(std::min(options.threads, options.clients)));
    return 0;
}

} // namespace
} // namespace crowdbook

int main(int argc, char** argv) {
    try {
        return crowdbook::run(std::span{argv, static_cast<std::size_t>(argc)});
    } catch (const std::exception& error) {
        std::cerr << "crowdbook_load: " << error.what() << '\n';
        return 1;
    }
}

#include "connect.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ladder.hpp"
#include "remote.hpp"
#include "terminal.hpp"
#include "trading_screen.hpp"

namespace crowdbook {

namespace {

constexpr auto kFrame = std::chrono::milliseconds{33};

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// A non-blocking connection to the server that sends and receives whole lines.
class Connection {
public:
    Connection(const std::string& host, std::uint16_t port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICSERV;
        addrinfo* found = nullptr;
        const std::string service = std::to_string(port);
        if (const int error = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &found);
            error != 0) {
            throw std::runtime_error(
                std::format("cannot reach {}:{}: {}", host, port, ::gai_strerror(error)));
        }
        std::string failure = "no address";
        for (addrinfo* address = found; address != nullptr && fd_ < 0;
             address = address->ai_next) {
            const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (fd < 0) {
                failure = std::strerror(errno);
            } else if (::connect(fd, address->ai_addr, address->ai_addrlen) < 0) {
                failure = std::strerror(errno);
                ::close(fd);
            } else {
                fd_ = fd;
            }
        }
        ::freeaddrinfo(found);
        if (fd_ < 0) {
            throw std::runtime_error(std::format("cannot connect to {}:{}: {}", host, port,
                                                 failure));
        }
        ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
        const int on = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
#ifdef SO_NOSIGPIPE
        ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection() { ::close(fd_); }

    [[nodiscard]] bool open() const noexcept { return open_; }

    void send(const protocol::ClientMessage& message) {
        outgoing_ += protocol::encode(message);
        flush();
    }

    // Sends what it can of what is waiting.
    void flush() {
        while (open_ && !outgoing_.empty()) {
            const ssize_t sent = ::send(fd_, outgoing_.data(), outgoing_.size(), kSendFlags);
            if (sent > 0) {
                outgoing_.erase(0, static_cast<std::size_t>(sent));
            } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                return;
            } else {
                open_ = false;
            }
        }
    }

    // Every complete line that has arrived, without waiting.
    std::vector<std::string> receive() {
        char chunk[64 * 1024];
        while (open_) {
            const ssize_t received = ::recv(fd_, chunk, sizeof chunk, 0);
            if (received > 0) {
                incoming_.append(chunk, static_cast<std::size_t>(received));
            } else if (received < 0 &&
                       (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                break;
            } else {
                open_ = false;
            }
        }
        std::vector<std::string> lines;
        std::size_t start = 0;
        for (std::size_t end = incoming_.find('\n'); end != std::string::npos;
             end = incoming_.find('\n', start)) {
            lines.push_back(incoming_.substr(start, end - start + 1));
            start = end + 1;
        }
        incoming_.erase(0, start);
        return lines;
    }

private:
    int fd_ = -1;
    bool open_ = true;
    std::string incoming_;
    std::string outgoing_;
};

// Hands the screen every line that has arrived.
void receive(Connection& connection, TradingScreen& screen) {
    for (const std::string& line : connection.receive()) {
        screen.receive(line);
    }
}

// Waits up to `timeout` for the server's welcome. Returns whether it came.
bool awaitWelcome(Connection& connection, TradingScreen& screen,
                  std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!screen.remote().welcomed() && connection.open() &&
           std::chrono::steady_clock::now() < deadline) {
        receive(connection, screen);
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return screen.remote().welcomed();
}

// Runs until the person quits.
void trade(RawTerminal& terminal, Connection& connection, TradingScreen& screen) {
    std::vector<protocol::ClientMessage> sent;
    while (true) {
        receive(connection, screen);
        const auto [rows, columns] = terminal.size();
        terminal.draw(ladder::render(screen.screen(rows, columns, connection.open())));
        for (std::optional<Key> key = terminal.readKey(kFrame); key;
             key = terminal.readKey(std::chrono::milliseconds{0})) {
            if (!screen.press(*key, sent)) {
                return;
            }
            for (const protocol::ClientMessage& message : sent) {
                connection.send(message);
            }
            sent.clear();
        }
        connection.flush();
    }
}

} // namespace

int connect(const ConnectOptions& options) {
    Connection connection{options.host, options.port};
    connection.send(protocol::Hello{.seat = options.seat, .token = options.token});
    TradingScreen screen{options.seat};
    if (!awaitWelcome(connection, screen, std::chrono::seconds{10})) {
        const std::string& message = screen.remote().message();
        throw std::runtime_error(message.empty() ? "the server did not answer"
                                                 : std::format("{}", message));
    }
    if (const auto& challenge = screen.remote().settings().challenge) {
        std::cout << std::format("challenge: {}\n\n{}\n\n", challenge->name,
                                 challenge->briefing);
        if (::isatty(STDIN_FILENO) != 0) {
            std::cout << "press enter to start" << std::flush;
            std::string line;
            std::getline(std::cin, line);
        }
    }
    {
        RawTerminal terminal;
        trade(terminal, connection, screen);
    }
    const RemoteMarket& remote = screen.remote();
    if (remote.end()) {
        const protocol::End& end = *remote.end();
        std::cout << std::format("{}:{} as {}: the session ended; position {:+}, cash {}, "
                                 "pnl {:+}, fees {}\n",
                                 options.host, options.port, options.seat, end.position,
                                 end.cash, end.pnl, formatFee(end.fees));
    } else if (remote.welcomed()) {
        std::cout << std::format("{}:{} as {}: left at {:.1f}s with position {:+} and cash {}\n",
                                 options.host, options.port, options.seat,
                                 static_cast<double>(remote.now()) / 1e9,
                                 remote.ledger().position(), remote.ledger().cash());
    } else if (!remote.message().empty()) {
        std::cout << remote.message() << '\n';
    }
    return 0;
}

} // namespace crowdbook

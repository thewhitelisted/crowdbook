#include "connect.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <variant>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ladder.hpp"
#include "remote.hpp"
#include "terminal.hpp"

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

class Client {
public:
    Client(Connection& connection, std::string seat)
        : connection_(connection), seat_(std::move(seat)) {}

    // Waits up to `timeout` for the server's welcome. Returns whether it came.
    bool awaitWelcome(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!remote_.welcomed() && connection_.open() &&
               std::chrono::steady_clock::now() < deadline) {
            receive();
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return remote_.welcomed();
    }

    // Runs until the person quits.
    void run(RawTerminal& terminal) {
        while (true) {
            receive();
            if (remote_.welcomed() && !placed_) {
                cursor_ = remote_.settings().referencePrice;
                size_ = std::min<Quantity>(5, remote_.settings().account.maxOrderQuantity);
                placed_ = true;
            }
            terminal.draw(ladder::render(screen(terminal)));
            for (std::optional<Key> key = terminal.readKey(kFrame); key;
                 key = terminal.readKey(std::chrono::milliseconds{0})) {
                if (!handle(*key)) {
                    return;
                }
            }
            connection_.flush();
        }
    }

    [[nodiscard]] const RemoteMarket& remote() const noexcept { return remote_; }

private:
    void receive() {
        for (const std::string& line : connection_.receive()) {
            try {
                remote_.apply(protocol::decodeServer(line));
            } catch (const protocol::ProtocolError& error) {
                message_ =
                    std::format("a message from the server made no sense: {}", error.what());
            }
            noteServerMessage();
        }
    }

    // The latest message from the server replaces this screen's own.
    void noteServerMessage() {
        if (remote_.message() != shown_) {
            shown_ = remote_.message();
            message_ = shown_;
        }
    }

    [[nodiscard]] std::string status() const {
        if (!connection_.open() && !remote_.end()) {
            return message_.starts_with("server:")
                       ? message_
                       : "the connection to the server is closed: press q";
        }
        if (!message_.empty()) {
            return message_;
        }
        if (!remote_.welcomed()) {
            return "waiting for the server";
        }
        if (!remote_.started()) {
            return "waiting for every seat to be claimed";
        }
        return "";
    }

    [[nodiscard]] ladder::Screen screen(const RawTerminal& terminal) const {
        const auto [rows, columns] = terminal.size();
        ladder::Screen view;
        view.seat = seat_;
        view.now = remote_.now();
        view.rows = rows;
        view.columns = columns;
        view.cursor = cursor_;
        view.size = size_;
        if (remote_.welcomed()) {
            const protocol::Welcome& settings = remote_.settings();
            view.end = settings.duration;
            view.referencePrice = settings.referencePrice;
            view.initialCash = settings.account.initialCash;
            view.initialPosition = settings.account.initialPosition;
            view.market = remote_.market();
            view.ledger = &remote_.ledger();
            view.tape.assign(remote_.tape().begin(), remote_.tape().end());
        }
        view.message = status();
        return view;
    }

    [[nodiscard]] bool canTrade() const {
        return connection_.open() && remote_.started() && !remote_.end();
    }

    void order(Side side, OrderType type) {
        if (!canTrade()) {
            return;
        }
        connection_.send(remote_.order(side, type, cursor_, size_));
        message_ = type == OrderType::Market
                       ? std::format("{} {} at the market", side == Side::Buy ? "buy" : "sell",
                                     size_)
                       : std::format("{} {} at {}", side == Side::Buy ? "bid" : "offer", size_,
                                     cursor_);
    }

    void cancel(bool everywhere) {
        if (!canTrade()) {
            return;
        }
        const std::vector<CancelOrder> cancels =
            remote_.cancels(everywhere ? std::nullopt : std::optional<Price>{cursor_});
        for (const CancelOrder& cancel : cancels) {
            connection_.send(cancel);
        }
        message_ = std::format("cancelling {} order{}", cancels.size(),
                               cancels.size() == 1 ? "" : "s");
    }

    void centerCursor() {
        const MarketSnapshot& market = remote_.market();
        if (market.bid && market.ask) {
            cursor_ = (market.bid->price + market.ask->price) / 2;
        } else if (market.lastTrade) {
            cursor_ = *market.lastTrade;
        }
    }

    // Returns false when the person quits.
    bool handle(const Key& key) {
        switch (key.kind) {
        case Key::Kind::Up:
            ++cursor_;
            return true;
        case Key::Kind::Down:
            cursor_ = std::max<Price>(1, cursor_ - 1);
            return true;
        case Key::Kind::PageUp:
            cursor_ += 10;
            return true;
        case Key::Kind::PageDown:
            cursor_ = std::max<Price>(1, cursor_ - 10);
            return true;
        case Key::Kind::Character:
            break;
        }
        const Quantity largest =
            remote_.welcomed() ? remote_.settings().account.maxOrderQuantity : 1;
        switch (key.character) {
        case 'q':
            return false;
        case 'b':
            order(Side::Buy, OrderType::Limit);
            break;
        case 's':
            order(Side::Sell, OrderType::Limit);
            break;
        case 'B':
            order(Side::Buy, OrderType::Market);
            break;
        case 'S':
            order(Side::Sell, OrderType::Market);
            break;
        case 'c':
            cancel(false);
            break;
        case 'C':
            cancel(true);
            break;
        case 'm':
            centerCursor();
            break;
        case '+':
        case '=':
            size_ = std::min(size_ + 1, largest);
            break;
        case '-':
        case '_':
            size_ = std::max<Quantity>(size_ - 1, 1);
            break;
        default:
            break;
        }
        return true;
    }

    Connection& connection_;
    std::string seat_;
    RemoteMarket remote_;
    bool placed_ = false;
    Price cursor_ = 1;
    Quantity size_ = 1;
    std::string message_; // the latest thing to tell the person
    std::string shown_;   // the server's latest message, once noted
};

} // namespace

int connect(const ConnectOptions& options) {
    Connection connection{options.host, options.port};
    connection.send(protocol::Hello{.seat = options.seat, .token = options.token});
    Client client{connection, options.seat};
    if (!client.awaitWelcome(std::chrono::seconds{10})) {
        const std::string& message = client.remote().message();
        throw std::runtime_error(message.empty() ? "the server did not answer"
                                                 : std::format("{}", message));
    }
    if (const auto& challenge = client.remote().settings().challenge) {
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
        client.run(terminal);
    }
    const RemoteMarket& remote = client.remote();
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

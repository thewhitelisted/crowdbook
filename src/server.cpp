#include "crowdbook/server.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace crowdbook {

namespace {

// How long poll waits for something to happen before the market is run on. Short, so that the
// market's clock and the clients' messages stay within a millisecond of the wall clock.
constexpr int kPollMilliseconds = 1;
constexpr std::size_t kReadSize = 64 * 1024;

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0; // macOS: SO_NOSIGPIPE on each socket instead
#endif

std::int64_t wallNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string lastError() {
    return std::strerror(errno);
}

// A file descriptor, closed when it goes.
class Socket {
public:
    explicit Socket(int fd = -1) noexcept : fd_(fd) {}
    Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { reset(); }

    [[nodiscard]] int fd() const noexcept { return fd_; }

    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_;
};

void configure(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw NetworkError(std::format("cannot make a socket non-blocking: {}", lastError()));
    }
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
}

Socket listenOn(const ServerOptions& options, std::uint16_t& port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    addrinfo* found = nullptr;
    const std::string service = std::to_string(options.port);
    if (const int error = ::getaddrinfo(options.host.c_str(), service.c_str(), &hints, &found);
        error != 0) {
        throw NetworkError(std::format("cannot listen on {}:{}: {}", options.host, options.port,
                                       ::gai_strerror(error)));
    }
    std::string failure = "no address";
    for (addrinfo* address = found; address != nullptr; address = address->ai_next) {
        Socket socket{::socket(address->ai_family, address->ai_socktype, address->ai_protocol)};
        if (socket.fd() < 0) {
            failure = lastError();
            continue;
        }
        const int on = 1;
        ::setsockopt(socket.fd(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        if (::bind(socket.fd(), address->ai_addr, address->ai_addrlen) < 0 ||
            ::listen(socket.fd(), SOMAXCONN) < 0) {
            failure = lastError();
            continue;
        }
        sockaddr_storage bound{};
        socklen_t length = sizeof bound;
        if (::getsockname(socket.fd(), reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
            port = ntohs(bound.ss_family == AF_INET6
                             ? reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port
                             : reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
        }
        ::freeaddrinfo(found);
        configure(socket.fd());
        return socket;
    }
    ::freeaddrinfo(found);
    throw NetworkError(
        std::format("cannot listen on {}:{}: {}", options.host, options.port, failure));
}

} // namespace

void serve(Gateway& gateway, const ServerOptions& options, const std::atomic<bool>& stop,
           const std::function<void(std::uint16_t port)>& onListening) {
    std::uint16_t port = options.port;
    Socket listener = listenOn(options, port);
    if (onListening) {
        onListening(port);
    }
    std::map<ConnectionId, Socket> connections;
    // Connections the gateway is done with, closed for writing, read and discarded until the
    // client hangs up or the grace runs out. Closing a socket with the client's messages still
    // unread would reset the connection, and a reset can lose the last messages sent to it.
    std::vector<std::pair<Socket, std::int64_t>> lingering;
    std::optional<std::int64_t> finishedAt;
    std::vector<pollfd> polled;
    std::vector<ConnectionId> polledIds;
    std::array<char, kReadSize> buffer{};

    const auto drop = [&](ConnectionId id, std::int64_t wall) {
        connections.erase(id);
        gateway.disconnect(id, wall);
    };
    // Lets the gateway go of a connection whose last messages are sent, and lingers on it.
    const auto finish = [&](ConnectionId id, std::int64_t wall) {
        const auto found = connections.find(id);
        ::shutdown(found->second.fd(), SHUT_WR);
        lingering.emplace_back(std::move(found->second), wall + options.closeGrace);
        drop(id, wall);
    };

    while (true) {
        std::int64_t wall = wallNow();
        if (stop.load() && !gateway.finished()) {
            gateway.stop(wall);
        }
        gateway.advance(wall);
        if (gateway.finished() && !finishedAt) {
            finishedAt = wall;
            listener.reset();
        }

        // Send what is waiting, and close what should be closed.
        std::vector<ConnectionId> broken;
        std::vector<ConnectionId> done;
        for (auto& [id, socket] : connections) {
            const std::string_view pending = gateway.pendingOutput(id);
            if (!pending.empty()) {
                const ssize_t sent =
                    ::send(socket.fd(), pending.data(), pending.size(), kSendFlags);
                if (sent > 0) {
                    gateway.consumeOutput(id, static_cast<std::size_t>(sent));
                } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    broken.push_back(id);
                    continue;
                }
            }
            if (gateway.shouldClose(id) ||
                (finishedAt && wall - *finishedAt >= options.closeGrace)) {
                done.push_back(id);
            }
        }
        for (const ConnectionId id : broken) {
            drop(id, wall);
        }
        for (const ConnectionId id : done) {
            finish(id, wall);
        }
        // Read and discard what lingering clients still send, until they hang up.
        std::erase_if(lingering, [&](std::pair<Socket, std::int64_t>& entry) {
            while (true) {
                const ssize_t received = ::recv(entry.first.fd(), buffer.data(), buffer.size(), 0);
                if (received > 0) {
                    continue;
                }
                if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                    return wall >= entry.second;
                }
                return true; // hung up, or the connection broke
            }
        });
        if (finishedAt && connections.empty() && lingering.empty()) {
            return;
        }

        polled.clear();
        polledIds.clear();
        if (listener.fd() >= 0) {
            polled.push_back({.fd = listener.fd(), .events = POLLIN, .revents = 0});
            polledIds.push_back(0);
        }
        for (const auto& [id, socket] : connections) {
            const auto events = static_cast<short>(
                POLLIN | (gateway.pendingOutput(id).empty() ? 0 : POLLOUT));
            polled.push_back({.fd = socket.fd(), .events = events, .revents = 0});
            polledIds.push_back(id);
        }
        if (::poll(polled.data(), static_cast<nfds_t>(polled.size()), kPollMilliseconds) < 0 &&
            errno != EINTR) {
            throw NetworkError(std::format("poll failed: {}", lastError()));
        }

        wall = wallNow();
        for (std::size_t i = 0; i < polled.size(); ++i) {
            const short events = polled[i].revents;
            if (events == 0) {
                continue;
            }
            if (polledIds[i] == 0) {
                // New connections, as many as are waiting.
                while (true) {
                    Socket accepted{::accept(listener.fd(), nullptr, nullptr)};
                    if (accepted.fd() < 0) {
                        break;
                    }
                    configure(accepted.fd());
                    const int on = 1;
                    ::setsockopt(accepted.fd(), IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
                    connections.emplace(gateway.connect(wall), std::move(accepted));
                }
                continue;
            }
            const ConnectionId id = polledIds[i];
            const auto found = connections.find(id);
            if (found == connections.end()) {
                continue;
            }
            if ((events & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }
            const ssize_t received = ::recv(found->second.fd(), buffer.data(), buffer.size(), 0);
            if (received > 0) {
                gateway.receive(id,
                                std::string_view{buffer.data(), static_cast<std::size_t>(received)},
                                wall);
            } else if (received == 0 ||
                       (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                drop(id, wall);
            }
        }
    }
}

} // namespace crowdbook

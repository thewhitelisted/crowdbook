#include "crowdbook/server.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/prctl.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
    defined(__DragonFly__)
#include <sys/event.h>
#else
#error "the server needs epoll (Linux) or kqueue (macOS and the BSDs)"
#endif

namespace crowdbook {

namespace {

constexpr std::size_t kReadSize = 64 * 1024;
// The longest the server waits for anything, so that it notices `stop` soon.
constexpr std::int64_t kLongestWait = 50 * kMillisecond;
// Waits shorter than this are not worth putting the thread to sleep for: it checks for input and
// carries on.
constexpr std::int64_t kShortestSleep = 20 * kMicrosecond;
// The poller's tags: the listener, connections by their ids, and lingering connections.
constexpr std::uint64_t kListener = 0;
constexpr std::uint64_t kLingering = std::uint64_t{1} << 63;

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

// Waits for sockets to have something to read, or room to write, or for a deadline: epoll on
// Linux, kqueue on macOS and the BSDs. Both wait to the nanosecond, or close to it, where poll
// would round to a millisecond. A socket leaves the poller when it is closed.
class Poller {
public:
    struct Ready {
        std::uint64_t tag = 0;
        bool readable = false; // or hung up, which reading finds out
        bool writable = false;
    };

    Poller() : fd_(create()) {
        if (fd_.fd() < 0) {
            throw NetworkError(std::format("cannot wait for sockets: {}", lastError()));
        }
    }

    // Watches the socket for something to read, under `tag`; for a socket already watched, gives
    // it the new tag and stops watching it for room to write.
    void watch(int fd, std::uint64_t tag, bool already = false) {
#if defined(__linux__)
        epoll_event event{.events = EPOLLIN | EPOLLRDHUP, .data = {.u64 = tag}};
        check(::epoll_ctl(fd_.fd(), already ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &event));
#else
        std::array<struct kevent, 2> changes{};
        EV_SET(&changes[0], fd, EVFILT_READ, EV_ADD, 0, 0, reinterpret_cast<void*>(tag));
        EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        // Deleting a write filter that is not there fails harmlessly, so its result is ignored.
        check(::kevent(fd_.fd(), changes.data(), 1, nullptr, 0, nullptr));
        if (already) {
            ::kevent(fd_.fd(), &changes[1], 1, nullptr, 0, nullptr);
        }
#endif
    }

    // Starts or stops watching the socket for room to write.
    void watchWritable(int fd, std::uint64_t tag, bool watching) {
#if defined(__linux__)
        epoll_event event{
            .events = static_cast<std::uint32_t>(EPOLLIN | EPOLLRDHUP | (watching ? EPOLLOUT : 0)),
            .data = {.u64 = tag}};
        check(::epoll_ctl(fd_.fd(), EPOLL_CTL_MOD, fd, &event));
#else
        struct kevent change {};
        EV_SET(&change, fd, EVFILT_WRITE, watching ? EV_ADD : EV_DELETE, 0, 0,
               reinterpret_cast<void*>(tag));
        check(::kevent(fd_.fd(), &change, 1, nullptr, 0, nullptr));
#endif
    }

    // Waits up to `timeout` nanoseconds for something to be ready, and returns what is.
    std::span<const Ready> wait(std::int64_t timeout) {
        ready_.clear();
        const timespec limit{.tv_sec = static_cast<time_t>(timeout / kSecond),
                             .tv_nsec = static_cast<long>(timeout % kSecond)};
#if defined(__linux__)
        int count = -1;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 35))
        count = ::epoll_pwait2(fd_.fd(), events_.data(), static_cast<int>(events_.size()), &limit,
                               nullptr);
        if (count < 0 && errno == ENOSYS) // a kernel older than 5.11
#endif
        {
            const auto milliseconds = static_cast<int>((timeout + kMillisecond - 1) / kMillisecond);
            count = ::epoll_wait(fd_.fd(), events_.data(), static_cast<int>(events_.size()),
                                 milliseconds);
        }
#else
        const int count = ::kevent(fd_.fd(), nullptr, 0, events_.data(),
                                   static_cast<int>(events_.size()), &limit);
#endif
        if (count < 0) {
            if (errno == EINTR) {
                return ready_;
            }
            throw NetworkError(std::format("waiting for sockets failed: {}", lastError()));
        }
        for (int i = 0; i < count; ++i) {
            const auto& event = events_[static_cast<std::size_t>(i)];
#if defined(__linux__)
            ready_.push_back(
                {.tag = event.data.u64,
                 .readable = (event.events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0,
                 .writable = (event.events & (EPOLLOUT | EPOLLHUP | EPOLLERR)) != 0});
#else
            ready_.push_back({.tag = reinterpret_cast<std::uint64_t>(event.udata),
                              .readable = event.filter == EVFILT_READ,
                              .writable = event.filter == EVFILT_WRITE});
#endif
        }
        return ready_;
    }

private:
    static int create() {
#if defined(__linux__)
        return ::epoll_create1(EPOLL_CLOEXEC);
#else
        return ::kqueue();
#endif
    }

    static void check(int result) {
        if (result < 0) {
            throw NetworkError(std::format("cannot watch a socket: {}", lastError()));
        }
    }

    Socket fd_;
#if defined(__linux__)
    std::array<epoll_event, 256> events_{};
#else
    std::array<struct kevent, 256> events_{};
#endif
    std::vector<Ready> ready_;
};

// On Linux, a sleeping thread wakes up to 50 microseconds late by default, to save power; the
// server wants its wakeups on time while it serves.
class OnTime {
public:
#if defined(__linux__)
    OnTime() : before_(::prctl(PR_GET_TIMERSLACK, 0UL, 0UL, 0UL, 0UL)) {
        ::prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
    }
    ~OnTime() {
        if (before_ > 0) {
            ::prctl(PR_SET_TIMERSLACK, static_cast<unsigned long>(before_), 0UL, 0UL, 0UL);
        }
    }

private:
    int before_;
#endif
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
    Poller poller;
    poller.watch(listener.fd(), kListener);
    // Held market data is sent as the latest of each kind, not every snapshot in between.
    gateway.conflate(options.feedInterval > 0);
    if (onListening) {
        onListening(port);
    }
    [[maybe_unused]] const OnTime onTime;

    struct Open {
        Socket socket;
        bool watchingWrites = false; // a send would block, so the poller says when it will not
        bool held = false;           // market data held back until the next feed tick
    };
    std::map<ConnectionId, Open> connections;
    // Connections the gateway is done with, closed for writing, read and discarded until the
    // client hangs up or the grace runs out. Closing a socket with the client's messages still
    // unread would reset the connection, and a reset can lose the last messages sent to it.
    struct Lingering {
        Socket socket;
        std::int64_t until = 0;
    };
    std::map<std::uint64_t, Lingering> lingering;
    std::uint64_t nextLingering = 0;
    std::optional<std::int64_t> finishedAt;
    bool graceOver = false;
    std::vector<ConnectionId> ready;
    // Connections with market data held back, and when it next goes.
    std::vector<ConnectionId> held;
    std::int64_t nextFeed = 0;
    std::array<char, kReadSize> buffer{};

    const auto drop = [&](ConnectionId id, std::int64_t wall) {
        connections.erase(id);
        gateway.disconnect(id, wall);
    };
    // Lets the gateway go of a connection whose last messages are sent, and lingers on it.
    const auto finish = [&](ConnectionId id, std::int64_t wall) {
        const auto found = connections.find(id);
        Socket socket = std::move(found->second.socket);
        ::shutdown(socket.fd(), SHUT_WR);
        const std::uint64_t tag = kLingering | nextLingering++;
        poller.watch(socket.fd(), tag, true);
        lingering.emplace(tag, Lingering{std::move(socket), wall + options.closeGrace});
        drop(id, wall);
    };
    // Sends what the connection has waiting, as much as the socket takes, and finishes it if the
    // gateway is done with it.
    const auto flush = [&](ConnectionId id, std::int64_t wall) {
        const auto found = connections.find(id);
        if (found == connections.end()) {
            return;
        }
        Open& open = found->second;
        for (std::string_view pending = gateway.pendingOutput(id); !pending.empty();
             pending = gateway.pendingOutput(id)) {
            const ssize_t sent =
                ::send(open.socket.fd(), pending.data(), pending.size(), kSendFlags);
            if (sent > 0) {
                gateway.consumeOutput(id, static_cast<std::size_t>(sent));
            } else if (sent < 0 && errno == EINTR) {
                continue;
            } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else {
                drop(id, wall); // the connection broke
                return;
            }
        }
        const bool blocked = !gateway.pendingOutput(id).empty();
        if (blocked != open.watchingWrites) {
            poller.watchWritable(open.socket.fd(), id, blocked);
            open.watchingWrites = blocked;
        }
        if (gateway.shouldClose(id)) {
            finish(id, wall);
        }
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
        ready.clear();
        gateway.takeReady(ready);
        for (const ConnectionId id : ready) {
            const auto found = connections.find(id);
            if (options.feedInterval == 0 || found == connections.end() || gateway.urgent(id) ||
                gateway.shouldClose(id)) {
                flush(id, wall);
            } else if (!found->second.held) {
                found->second.held = true;
                held.push_back(id);
            }
        }
        if (!held.empty() && wall >= nextFeed) {
            for (const ConnectionId id : held) {
                if (const auto found = connections.find(id); found != connections.end()) {
                    found->second.held = false;
                    flush(id, wall);
                }
            }
            held.clear();
        }
        if (wall >= nextFeed) {
            nextFeed = wall + options.feedInterval;
        }
        if (finishedAt && !graceOver && wall - *finishedAt >= options.closeGrace) {
            graceOver = true;
            std::vector<ConnectionId> remaining;
            for (const auto& [id, open] : connections) {
                remaining.push_back(id);
            }
            for (const ConnectionId id : remaining) {
                finish(id, wall);
            }
        }
        std::erase_if(lingering, [wall](const auto& entry) { return wall >= entry.second.until; });
        if (finishedAt && connections.empty() && lingering.empty()) {
            return;
        }

        // Sleep until the next moment something is due, unless something arrives first.
        std::int64_t deadline = wall + kLongestWait;
        if (const std::optional<std::int64_t> next = gateway.nextWake()) {
            deadline = std::min(deadline, *next);
        }
        for (const auto& [tag, entry] : lingering) {
            deadline = std::min(deadline, entry.until);
        }
        if (finishedAt && !graceOver) {
            deadline = std::min(deadline, *finishedAt + options.closeGrace);
        }
        if (!held.empty()) {
            deadline = std::min(deadline, nextFeed);
        }
        // With a spin, sleep only until shortly before the deadline, then check without sleeping
        // until it comes.
        std::int64_t timeout = deadline - wallNow() - options.spin;
        if (timeout < kShortestSleep) {
            timeout = 0;
        }
        const std::span<const Poller::Ready> events = poller.wait(timeout);
        wall = wallNow();
        for (const Poller::Ready& event : events) {
            if (event.tag == kListener) {
                // New connections, as many as are waiting.
                while (listener.fd() >= 0) {
                    Socket accepted{::accept(listener.fd(), nullptr, nullptr)};
                    if (accepted.fd() < 0) {
                        break;
                    }
                    configure(accepted.fd());
                    const int on = 1;
                    ::setsockopt(accepted.fd(), IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
                    const ConnectionId id = gateway.connect(wall);
                    poller.watch(accepted.fd(), id);
                    connections.emplace(id, Open{.socket = std::move(accepted)});
                }
            } else if ((event.tag & kLingering) != 0) {
                // Read and discard what a lingering client still sends, until it hangs up.
                const auto found = lingering.find(event.tag);
                if (found == lingering.end()) {
                    continue;
                }
                const ssize_t received =
                    ::recv(found->second.socket.fd(), buffer.data(), buffer.size(), 0);
                if (received == 0 ||
                    (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    lingering.erase(found);
                }
            } else {
                const ConnectionId id = event.tag;
                if (event.writable) {
                    flush(id, wall);
                }
                const auto found = connections.find(id);
                if (!event.readable || found == connections.end()) {
                    continue;
                }
                const ssize_t received =
                    ::recv(found->second.socket.fd(), buffer.data(), buffer.size(), 0);
                if (received > 0) {
                    gateway.receive(
                        id, std::string_view{buffer.data(), static_cast<std::size_t>(received)},
                        wall);
                } else if (received == 0 ||
                           (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    drop(id, wall);
                }
            }
        }
    }
}

} // namespace crowdbook

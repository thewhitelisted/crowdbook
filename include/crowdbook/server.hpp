#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

#include "crowdbook/gateway.hpp"

namespace crowdbook {

// A socket that could not be opened, or failed in a way the server cannot carry on from.
class NetworkError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ServerOptions {
    std::string host = "127.0.0.1"; // the address to listen on
    std::uint16_t port = 7878;      // 0 picks a free port
    // How long, in wall-clock nanoseconds, clients get to read their last messages after the
    // session ends before their connections are closed anyway.
    std::int64_t closeGrace = 2 * kSecond;
    // How long, in wall-clock nanoseconds, before something is due the server stops sleeping and
    // watches the clock instead. A sleeping thread wakes a little late, up to a tenth of a
    // millisecond on macOS, so this buys messages that leave on time to the microsecond, at the
    // cost of up to this much busy waiting before every event. 0 sleeps until each is due.
    std::int64_t spin = 0;
    // How often, in wall-clock nanoseconds, market data is sent: every seat's at once, at each
    // tick, so that none sees the market before another. A seat's own order events, errors and the
    // start and end go at once, with whatever is waiting. 0 sends everything as it happens.
    // Batching costs a seat up to this much delay in market data, and saves a system call per
    // connection for every change in the market.
    std::int64_t feedInterval = 0;
};

// Serves the gateway's market over TCP until its session is over and every connection is closed,
// or `stop` becomes true, which ends the session at once, within 50 milliseconds. Runs on the
// calling thread, with non-blocking sockets and epoll on Linux or kqueue on macOS and the BSDs.
// It sleeps until the market's next event is due or a client sends something, so messages leave
// when the market makes them. `onListening` is called with the port once the server is listening.
// Throws NetworkError if it cannot listen.
void serve(Gateway& gateway, const ServerOptions& options, const std::atomic<bool>& stop,
           const std::function<void(std::uint16_t port)>& onListening = {});

} // namespace crowdbook

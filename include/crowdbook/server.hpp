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
};

// Serves the gateway's market over TCP until its session is over and every connection is closed,
// or `stop` becomes true, which ends the session at once. Runs on the calling thread, with
// non-blocking POSIX sockets and poll. `onListening` is called with the port once the server is
// listening. Throws NetworkError if it cannot listen.
void serve(Gateway& gateway, const ServerOptions& options, const std::atomic<bool>& stop,
           const std::function<void(std::uint16_t port)>& onListening = {});

} // namespace crowdbook

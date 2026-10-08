#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "live_session.hpp"
#include "output.hpp"

namespace crowdbook {

struct ServeOptions {
    LiveOptions live{}; // the path is a scenario, or with rewindAt a session
    double speed = 1.0;
    std::vector<std::string> seats{}; // one seat, "you", if none are named
    std::string host = "127.0.0.1";
    std::uint16_t port = 7878;
    std::optional<std::string> tokensPath{}; // lines of "seat token"
    std::optional<std::int64_t> rateLimit{}; // messages per second per connection
};

// Serves the scenario's market over the network to clients speaking docs/protocol.md, until its
// time is up or the process is interrupted, then prints the results. Returns the exit code.
int serve(const ServeOptions& options);

// Reads "host:port", "[ipv6]:port" or ":port" (every address). Throws UsageError.
void parseListen(std::string_view text, std::string& host, std::uint16_t& port);

} // namespace crowdbook

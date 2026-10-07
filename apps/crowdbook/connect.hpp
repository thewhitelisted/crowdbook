#pragma once

#include <cstdint>
#include <string>

namespace crowdbook {

struct ConnectOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 7878;
    std::string seat = "you";
    std::string token{}; // none if empty
};

// Trades in a served market from the terminal trading screen, as the client of one seat, until
// the person quits or the session ends. Returns the exit code.
int connect(const ConnectOptions& options);

} // namespace crowdbook

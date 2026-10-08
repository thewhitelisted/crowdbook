#include "serve.hpp"

#include <atomic>
#include <charconv>
#include <csignal>
#include <format>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include "crowdbook/gateway.hpp"
#include "crowdbook/protocol.hpp"
#include "crowdbook/server.hpp"

namespace crowdbook {

namespace {

std::atomic<bool> interrupted{false};

extern "C" void onInterrupt(int /*signal*/) {
    interrupted = true;
}

// Each line names a seat and its token, separated by whitespace; blank lines and lines starting
// with '#' are skipped.
std::map<std::string, std::string> readTokens(const std::string& path) {
    std::istringstream lines{readFile(path)};
    std::map<std::string, std::string> tokens;
    std::string line;
    for (int number = 1; std::getline(lines, line); ++number) {
        std::istringstream words{line};
        std::string seat;
        std::string token;
        std::string extra;
        if (!(words >> seat) || seat.starts_with('#')) {
            continue;
        }
        if (!(words >> token) || (words >> extra) || token.size() > protocol::kMaxTokenLength) {
            throw std::runtime_error(
                std::format("{}:{}: each line needs a seat and a token, and nothing else", path,
                            number));
        }
        if (!tokens.emplace(seat, token).second) {
            throw std::runtime_error(
                std::format("{}:{}: the seat '{}' has two tokens", path, number, seat));
        }
    }
    return tokens;
}

std::string listSeats(const std::vector<std::string>& seats) {
    std::string list;
    for (const std::string& seat : seats) {
        list += (list.empty() ? "" : ", ") + seat;
    }
    return list;
}

} // namespace

void parseListen(std::string_view text, std::string& host, std::uint16_t& port) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        throw UsageError(std::format("--listen needs host:port, not '{}'", text));
    }
    std::string_view name = text.substr(0, colon);
    const std::string_view number = text.substr(colon + 1);
    if (name.starts_with('[') && name.ends_with(']')) {
        name = name.substr(1, name.size() - 2);
    }
    unsigned value = 0;
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), value);
    if (error != std::errc{} || end != number.data() + number.size() || value > 65'535) {
        throw UsageError(std::format("--listen needs a port from 0 to 65535, not '{}'", number));
    }
    host = name.empty() ? "0.0.0.0" : std::string{name};
    port = static_cast<std::uint16_t>(value);
}

int serve(const ServeOptions& options) {
    GatewayOptions gatewayOptions;
    if (!options.seats.empty()) {
        gatewayOptions.seats = options.seats;
    }
    if (options.tokensPath) {
        gatewayOptions.tokens = readTokens(*options.tokensPath);
    }
    gatewayOptions.speed = options.speed;
    if (options.rateLimit) {
        gatewayOptions.maxMessagesPerSecond = *options.rateLimit;
    }
    LiveSession live{options.live, std::move(gatewayOptions)};

    std::signal(SIGINT, onInterrupt);
    std::signal(SIGTERM, onInterrupt);
    crowdbook::serve(live.gateway(), {.host = options.host, .port = options.port}, interrupted,
                     [&](std::uint16_t port) {
                         std::cout << std::format(
                             "{} (seed {}): serving on {}:{} to seats {}; the clock starts when "
                             "every seat is claimed, and ctrl-c ends the session\n",
                             options.live.path, live.scenario().seed, options.host, port,
                             listSeats(live.seats()))
                                   << std::flush;
                     });
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
    live.finish(std::cout, "served");
    return 0;
}

} // namespace crowdbook

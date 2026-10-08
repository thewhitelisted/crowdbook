#include "play.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "crowdbook/protocol.hpp"
#include "crowdbook/session.hpp"
#include "ladder.hpp"
#include "terminal.hpp"
#include "trading_screen.hpp"

namespace crowdbook {

namespace {

constexpr std::array kSpeeds{0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 50.0};
constexpr auto kFrame = std::chrono::milliseconds{33};

std::int64_t wallNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double nextSpeed(double speed, bool faster) {
    if (faster) {
        const auto* next = std::ranges::find_if(kSpeeds, [speed](double s) { return s > speed; });
        return next == kSpeeds.end() ? speed : *next;
    }
    double slower = speed;
    for (const double candidate : kSpeeds) {
        if (candidate < speed) {
            slower = candidate;
        }
    }
    return slower;
}

// Hands the screen every line the gateway has for the connection.
void deliver(Gateway& gateway, ConnectionId connection, TradingScreen& screen) {
    const std::string_view pending = gateway.pendingOutput(connection);
    std::size_t start = 0;
    for (std::size_t end = pending.find('\n'); end != std::string_view::npos;
         end = pending.find('\n', start)) {
        screen.receive(pending.substr(start, end - start + 1));
        start = end + 1;
    }
    gateway.consumeOutput(connection, start);
}

// Trades as `seat` through the gateway, a client in the same process, until the person quits;
// then ends the session if it is still running.
void trade(RawTerminal& terminal, Gateway& gateway, const std::string& seat) {
    TradingScreen screen{
        seat, TradingScreen::Controls{
                  .changeSpeed =
                      [&gateway](bool faster) {
                          gateway.setSpeed(nextSpeed(gateway.speed(), faster), wallNow());
                      },
                  .togglePause = [&gateway] { gateway.setPaused(!gateway.paused(), wallNow()); },
                  .speed = [&gateway] { return gateway.speed(); },
                  .paused = [&gateway] { return gateway.paused(); }}};
    const ConnectionId connection = gateway.connect(wallNow());
    gateway.receive(connection, protocol::encode(protocol::Hello{.seat = seat}), wallNow());
    std::vector<protocol::ClientMessage> sent;
    while (true) {
        gateway.advance(wallNow());
        deliver(gateway, connection, screen);
        const auto [rows, columns] = terminal.size();
        terminal.draw(
            ladder::render(screen.screen(rows, columns, !gateway.shouldClose(connection))));
        // Wait a frame for a key, then take every other key already typed.
        for (std::optional<Key> key = terminal.readKey(kFrame); key;
             key = terminal.readKey(std::chrono::milliseconds{0})) {
            if (!screen.press(*key, sent)) {
                gateway.stop(wallNow());
                return;
            }
            for (const protocol::ClientMessage& message : sent) {
                gateway.receive(connection, protocol::encode(message), wallNow());
            }
            sent.clear();
        }
    }
}

} // namespace

int play(const PlayOptions& options) {
    if (options.live.rewindAt && loadSession(options.live.path).seats.size() != 1) {
        throw std::runtime_error("play takes over a session's only seat; serve a session with "
                                 "several seats to rewind it");
    }
    GatewayOptions gatewayOptions;
    gatewayOptions.speed = options.speed;
    // The screen redraws every frame, so the clock it shows should move as often.
    gatewayOptions.clockInterval =
        std::chrono::duration_cast<std::chrono::nanoseconds>(kFrame).count();
    LiveSession live{options.live, std::move(gatewayOptions)};
    if (live.scenario().challenge) {
        printBriefing(std::cout, live.scenario());
        if (::isatty(STDIN_FILENO) != 0) {
            std::cout << "press enter to start" << std::flush;
            std::string line;
            std::getline(std::cin, line);
        }
    }
    {
        RawTerminal terminal;
        trade(terminal, live.gateway(), live.seats().front());
    }
    live.finish(std::cout, "played");
    return 0;
}

} // namespace crowdbook

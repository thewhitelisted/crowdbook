#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "crowdbook/protocol.hpp"
#include "crowdbook/types.hpp"
#include "ladder.hpp"
#include "remote.hpp"
#include "terminal_codes.hpp"

namespace crowdbook {

// The trading screen for one seat, apart from any terminal and any connection: lines from the
// server go in, key presses come out as messages for the server, and screen() says what to draw.
// `crowdbook play` and `crowdbook connect` both run it, one against a gateway in the same
// process, the other over a socket.
class TradingScreen {
public:
    // For a session the person runs themselves: its clock's speed and pause.
    struct Controls {
        std::function<void(bool faster)> changeSpeed;
        std::function<void()> togglePause;
        std::function<double()> speed;
        std::function<bool()> paused;
    };

    explicit TradingScreen(std::string seat, std::optional<Controls> controls = std::nullopt);

    // Applies a line from the server; one that makes no sense is reported on the screen.
    void receive(std::string_view line);
    // Handles a key press, appending what it sends to `out`. Returns false when the person quits.
    bool press(const Key& key, std::vector<protocol::ClientMessage>& out);
    // What to draw on a terminal of this size; `connected` is false once the connection is gone.
    [[nodiscard]] ladder::Screen screen(std::size_t rows, std::size_t columns,
                                        bool connected) const;

    [[nodiscard]] const RemoteMarket& remote() const noexcept { return remote_; }

private:
    [[nodiscard]] bool canTrade() const;
    [[nodiscard]] std::string status(bool connected) const;
    void order(Side side, OrderType type, std::vector<protocol::ClientMessage>& out);
    void cancel(bool everywhere, std::vector<protocol::ClientMessage>& out);
    void centerCursor();
    [[nodiscard]] Price highest() const; // the highest price the exchange takes

    std::string seat_;
    std::optional<Controls> controls_;
    RemoteMarket remote_;
    bool placed_ = false; // the cursor and size have been set from the welcome
    Price cursor_ = 1;
    Quantity size_ = 1;
    std::string message_; // the latest thing to tell the person
    std::string shown_;   // the server's latest message, once noted
};

} // namespace crowdbook

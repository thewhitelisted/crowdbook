#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "crowdbook/agents/participant.hpp"
#include "crowdbook/ledger.hpp"
#include "crowdbook/messages.hpp"
#include "crowdbook/protocol.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// A served market as one client sees it, built only from what the server sends: the seat's own
// orders and balances, kept the way an agent's ledger is, and the public market. Orders are
// named by the client's ids.
class RemoteMarket {
public:
    explicit RemoteMarket(std::size_t tapeLength = 30);

    // Applies one message from the server. Throws std::logic_error for an order event about an
    // order this client never sent.
    void apply(const protocol::ServerMessage& message);

    // A new order with the next free id, recorded as sent; the caller sends it.
    [[nodiscard]] NewOrder order(Side side, OrderType type, Price price, Quantity quantity);
    // Cancels for the seat's limit orders at `price`, or at every price, recorded as sent; the
    // caller sends them.
    [[nodiscard]] std::vector<CancelOrder> cancels(std::optional<Price> price);

    [[nodiscard]] bool welcomed() const noexcept { return welcome_.has_value(); }
    [[nodiscard]] bool started() const noexcept { return started_; }
    // The welcome's settings: the market's length, reference price and the seat's account.
    // Only meaningful once welcomed.
    [[nodiscard]] const protocol::Welcome& settings() const { return *welcome_; }
    [[nodiscard]] Timestamp now() const noexcept { return now_; }
    [[nodiscard]] const Ledger& ledger() const noexcept { return ledger_; }
    [[nodiscard]] const MarketSnapshot& market() const noexcept { return market_; }
    [[nodiscard]] const std::deque<TapeEntry>& tape() const noexcept { return tape_; }
    [[nodiscard]] const std::optional<protocol::End>& end() const noexcept { return end_; }
    // The latest thing worth telling the person: a rejection, an error, the end.
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

private:
    std::size_t tapeLength_;
    std::optional<protocol::Welcome> welcome_;
    bool started_ = false;
    Timestamp now_ = 0;
    Ledger ledger_;
    ClientOrderId nextId_ = 1;
    MarketSnapshot market_;
    std::deque<TapeEntry> tape_;
    std::optional<protocol::End> end_;
    std::string message_;
};

} // namespace crowdbook

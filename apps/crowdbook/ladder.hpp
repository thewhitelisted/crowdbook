#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "remote.hpp"
#include "crowdbook/ledger.hpp"
#include "crowdbook/messages.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook::ladder {

// Everything the trading screen shows, as of one moment.
struct Screen {
    Timestamp now = 0;
    Duration end = 0;
    double speed = 1.0;
    bool paused = false;
    MarketSnapshot market{};           // as the participant sees it, one latency late
    const Ledger* ledger = nullptr;    // the participant's own balances and orders
    std::vector<TapeEntry> tape{};     // newest first
    Price referencePrice = 0;          // marks the position before anything trades
    Cash initialCash = 0;              // the account's starting balances, for the PnL
    Quantity initialPosition = 0;
    Price cursor = 0;                  // the price the keys buy and sell at
    Quantity size = 1;                 // lots per order
    std::string message{};             // the latest thing worth saying, such as a rejection
    // The seat, for a screen connected to a served market, which has no speed or pause to show.
    std::string seat{};
    std::size_t rows = 24;             // the terminal's size
    std::size_t columns = 80;
    bool color = true;                 // ANSI colors; off for tests and plain terminals
};

// The prices the ladder shows, highest first: `count` of them around the market's mid (else the
// last trade, else the reference price), shifted just enough to keep the cursor in view.
[[nodiscard]] std::vector<Price> visiblePrices(const Screen& screen, std::size_t count);

// The screen as text lines, at most `rows` of them, each at most `columns` wide not counting
// color codes.
[[nodiscard]] std::vector<std::string> render(const Screen& screen);

} // namespace crowdbook::ladder

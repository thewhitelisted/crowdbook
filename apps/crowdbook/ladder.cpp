#include "ladder.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <string_view>

namespace crowdbook::ladder {

namespace {

// Lines around the ladder: the status and the account, a blank, the column header, a blank, the
// tape, two lines of keys and the message.
constexpr std::size_t kFixedLines = 9;

std::string clock(Timestamp time) {
    const auto tenths = time / (kSecond / 10);
    return std::format("{:02}:{:02}.{}", tenths / 600, tenths / 10 % 60, tenths % 10);
}

std::optional<double> mid(const MarketSnapshot& market) {
    if (market.bid && market.ask) {
        return static_cast<double>(market.bid->price + market.ask->price) / 2.0;
    }
    return std::nullopt;
}

double markPrice(const Screen& screen) {
    if (const std::optional<double> middle = mid(screen.market)) {
        return *middle;
    }
    return static_cast<double>(screen.market.lastTrade.value_or(screen.referencePrice));
}

// Quantity at each price on one side: the book's levels as published, or just the best one when
// there is no depth feed.
std::map<Price, Quantity> bookSide(const std::vector<LevelSummary>& levels,
                                   const std::optional<LevelSummary>& best) {
    std::map<Price, Quantity> side;
    for (const LevelSummary& level : levels) {
        side[level.price] = level.quantity;
    }
    if (side.empty() && best) {
        side[best->price] = best->quantity;
    }
    return side;
}

std::map<Price, Quantity> ownSide(const Ledger* ledger, Side wanted) {
    std::map<Price, Quantity> side;
    if (ledger != nullptr) {
        for (const auto& [id, order] : ledger->orders()) {
            if (order.side == wanted && order.type == OrderType::Limit) {
                side[order.price] += order.leaves;
            }
        }
    }
    return side;
}

std::string cell(const std::map<Price, Quantity>& side, Price price) {
    const auto found = side.find(price);
    return found == side.end() ? std::string{} : std::to_string(found->second);
}

std::string signedText(double value, int decimals) {
    return std::format("{}{:.{}f}", value > 0 ? "+" : "", value, decimals);
}

// Wraps text in an ANSI color when colors are on.
std::string paint(const Screen& screen, std::string_view code, const std::string& text) {
    if (!screen.color || text.empty()) {
        return text;
    }
    return std::format("\x1b[{}m{}\x1b[0m", code, text);
}

std::string fit(std::string text, std::size_t columns) {
    if (text.size() > columns) {
        text.resize(columns);
    }
    return text;
}

} // namespace

std::vector<Price> visiblePrices(const Screen& screen, std::size_t count) {
    if (count == 0) {
        return {};
    }
    const auto half = static_cast<Price>(count / 2);
    Price top = static_cast<Price>(std::llround(markPrice(screen))) + half;
    // Shift the window just enough to keep the cursor on it.
    if (screen.cursor > top) {
        top = screen.cursor;
    } else if (screen.cursor <= top - static_cast<Price>(count)) {
        top = screen.cursor + static_cast<Price>(count) - 1;
    }
    std::vector<Price> prices;
    for (std::size_t i = 0; i < count && top - static_cast<Price>(i) >= 1; ++i) {
        prices.push_back(top - static_cast<Price>(i));
    }
    return prices;
}

std::vector<std::string> render(const Screen& screen) {
    std::vector<std::string> lines;
    const std::size_t width = screen.columns;

    if (screen.seat.empty()) {
        lines.push_back(fit(std::format("crowdbook  {} of {}  speed {}x  {}", clock(screen.now),
                                        clock(screen.end), screen.speed,
                                        screen.paused ? "PAUSED" : "running"),
                            width));
    } else {
        lines.push_back(fit(std::format("crowdbook  {} of {}  seat {}", clock(screen.now),
                                        clock(screen.end), screen.seat),
                            width));
    }
    if (screen.ledger != nullptr) {
        const Ledger& ledger = *screen.ledger;
        const double mark = markPrice(screen);
        const double fees = static_cast<double>(ledger.fees()) /
                            static_cast<double>(kFeeUnitsPerTickLot);
        const double pnl = static_cast<double>(ledger.cash() - screen.initialCash) +
                           static_cast<double>(ledger.position() - screen.initialPosition) * mark -
                           fees;
        lines.push_back(fit(std::format("position {:+}  cash {}  fees {:.1f}  pnl {} at {:.1f}",
                                        ledger.position(), ledger.cash(), fees,
                                        signedText(pnl, 1), mark),
                            width));
    } else {
        lines.emplace_back();
    }
    lines.emplace_back();
    lines.push_back(fit(std::format("  {:>9} {:>8} {:>8} {:>8} {:>9}", "your bids", "bids",
                                    "price", "asks", "your asks"),
                        width));

    const std::size_t ladderRows = screen.rows > kFixedLines ? screen.rows - kFixedLines : 1;
    const std::map<Price, Quantity> bids = bookSide(screen.market.bids, screen.market.bid);
    const std::map<Price, Quantity> asks = bookSide(screen.market.asks, screen.market.ask);
    const std::map<Price, Quantity> myBids = ownSide(screen.ledger, Side::Buy);
    const std::map<Price, Quantity> myAsks = ownSide(screen.ledger, Side::Sell);
    for (const Price price : visiblePrices(screen, ladderRows)) {
        const bool atCursor = price == screen.cursor;
        const bool lastTrade = screen.market.lastTrade == price;
        std::string row =
            std::format("{} {:>9} {:>8} {:>8}{} {:>8} {:>9}", atCursor ? ">" : " ",
                        cell(myBids, price), cell(bids, price), price, lastTrade ? "*" : " ",
                        cell(asks, price), cell(myAsks, price));
        row = fit(std::move(row), width);
        if (screen.color) {
            // Color by side, then mark the cursor row; the row's text is unchanged.
            const bool bidSide = bids.contains(price);
            const bool askSide = asks.contains(price);
            if (atCursor) {
                row = paint(screen, "7", row);
            } else if (bidSide) {
                row = paint(screen, "32", row);
            } else if (askSide) {
                row = paint(screen, "31", row);
            }
        }
        lines.push_back(std::move(row));
    }

    lines.emplace_back();
    std::string tape = "trades:";
    for (const TapeEntry& entry : screen.tape) {
        tape += std::format("  {} x{} {}", entry.trade.price, entry.trade.quantity,
                            entry.trade.aggressorSide == Side::Buy ? "up" : "down");
        if (tape.size() > width) {
            break;
        }
    }
    lines.push_back(fit(std::move(tape), width));
    lines.push_back(fit(std::format("arrows price  m mid  b bid  s offer  B buy now  S sell now  "
                                    "size {} (+/-)",
                                    screen.size),
                        width));
    lines.push_back(fit(screen.seat.empty()
                            ? "c cancel here  C cancel all  [ ] speed  space pause  q quit"
                            : "c cancel here  C cancel all  q quit",
                        width));
    lines.push_back(fit(screen.message, width));
    if (lines.size() > screen.rows) {
        lines.resize(screen.rows);
    }
    return lines;
}

} // namespace crowdbook::ladder

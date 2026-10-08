#include "trading_screen.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace crowdbook {

TradingScreen::TradingScreen(std::string seat, std::optional<Controls> controls)
    : seat_(std::move(seat)), controls_(std::move(controls)) {}

void TradingScreen::receive(std::string_view line) {
    try {
        remote_.apply(protocol::decodeServer(line));
    } catch (const protocol::ProtocolError& error) {
        message_ = std::format("a message from the server made no sense: {}", error.what());
    }
    // The latest message from the server replaces the screen's own.
    if (remote_.message() != shown_) {
        shown_ = remote_.message();
        message_ = shown_;
    }
    if (remote_.welcomed() && !placed_) {
        const protocol::Welcome& settings = remote_.settings();
        cursor_ = settings.referencePrice;
        size_ = std::min<Quantity>(5, settings.account.maxOrderQuantity);
        if (settings.depthLevels == 0 && message_.empty()) {
            message_ = "no depth feed in this market: the ladder shows only the best prices";
        }
        placed_ = true;
    }
}

bool TradingScreen::canTrade() const {
    return remote_.started() && !remote_.end();
}

std::string TradingScreen::status(bool connected) const {
    if (!connected && !remote_.end()) {
        return message_.starts_with("server:") ? message_
                                               : "the connection to the market is closed: "
                                                 "press q";
    }
    if (!message_.empty()) {
        return message_;
    }
    if (!remote_.welcomed()) {
        return "waiting for the market";
    }
    if (!remote_.started()) {
        return "waiting for every seat to be claimed";
    }
    return "";
}

ladder::Screen TradingScreen::screen(std::size_t rows, std::size_t columns,
                                     bool connected) const {
    // Filled in field by field: GCC 14 at -O3 mistakes the vectors of a braced temporary for
    // uninitialized.
    ladder::Screen view;
    // A session the person runs shows its speed and pause; one they joined shows their seat.
    if (controls_) {
        view.speed = controls_->speed();
        view.paused = controls_->paused();
    } else {
        view.seat = seat_;
    }
    view.now = remote_.now();
    view.rows = rows;
    view.columns = columns;
    view.cursor = cursor_;
    view.size = size_;
    if (remote_.welcomed()) {
        const protocol::Welcome& settings = remote_.settings();
        view.end = settings.duration;
        view.referencePrice = settings.referencePrice;
        view.initialCash = settings.account.initialCash;
        view.initialPosition = settings.account.initialPosition;
        view.market = remote_.market();
        view.ledger = &remote_.ledger();
        view.tape.assign(remote_.tape().begin(), remote_.tape().end());
    }
    view.message = status(connected);
    return view;
}

void TradingScreen::order(Side side, OrderType type, std::vector<protocol::ClientMessage>& out) {
    if (!canTrade()) {
        return;
    }
    out.emplace_back(remote_.order(side, type, cursor_, size_));
    message_ = type == OrderType::Market
                   ? std::format("{} {} at the market", side == Side::Buy ? "buy" : "sell", size_)
                   : std::format("{} {} at {}", side == Side::Buy ? "bid" : "offer", size_,
                                 cursor_);
}

void TradingScreen::cancel(bool everywhere, std::vector<protocol::ClientMessage>& out) {
    if (!canTrade()) {
        return;
    }
    const std::vector<CancelOrder> cancels =
        remote_.cancels(everywhere ? std::nullopt : std::optional<Price>{cursor_});
    out.insert(out.end(), cancels.begin(), cancels.end());
    message_ =
        std::format("cancelling {} order{}", cancels.size(), cancels.size() == 1 ? "" : "s");
}

Price TradingScreen::highest() const {
    return remote_.welcomed() ? remote_.settings().maxPrice : kMaxPrice;
}

void TradingScreen::centerCursor() {
    const MarketSnapshot& market = remote_.market();
    if (market.bid && market.ask) {
        cursor_ = (market.bid->price + market.ask->price) / 2;
    } else if (market.lastTrade) {
        cursor_ = *market.lastTrade;
    }
}

bool TradingScreen::press(const Key& key, std::vector<protocol::ClientMessage>& out) {
    switch (key.kind) {
    case Key::Kind::Up:
        cursor_ = std::min(cursor_ + 1, highest());
        return true;
    case Key::Kind::Down:
        cursor_ = std::max<Price>(1, cursor_ - 1);
        return true;
    case Key::Kind::PageUp:
        cursor_ = std::min(cursor_ + 10, highest());
        return true;
    case Key::Kind::PageDown:
        cursor_ = std::max<Price>(1, cursor_ - 10);
        return true;
    case Key::Kind::Character:
        break;
    }
    const Quantity largest = remote_.welcomed() ? remote_.settings().account.maxOrderQuantity : 1;
    switch (key.character) {
    case 'q':
        return false;
    case 'b':
        order(Side::Buy, OrderType::Limit, out);
        break;
    case 's':
        order(Side::Sell, OrderType::Limit, out);
        break;
    case 'B':
        order(Side::Buy, OrderType::Market, out);
        break;
    case 'S':
        order(Side::Sell, OrderType::Market, out);
        break;
    case 'c':
        cancel(false, out);
        break;
    case 'C':
        cancel(true, out);
        break;
    case 'm':
        centerCursor();
        break;
    case '+':
    case '=':
        size_ = std::min(size_ + 1, largest);
        break;
    case '-':
    case '_':
        size_ = std::max<Quantity>(size_ - 1, 1);
        break;
    case ' ':
        if (controls_) {
            controls_->togglePause();
        }
        break;
    case ']':
    case '[':
        if (controls_) {
            controls_->changeSpeed(key.character == ']');
        }
        break;
    default:
        break;
    }
    return true;
}

} // namespace crowdbook

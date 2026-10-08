#include "remote.hpp"

#include <algorithm>
#include <format>
#include <variant>

namespace crowdbook {

RemoteMarket::RemoteMarket(std::size_t tapeLength) : tapeLength_(tapeLength) {}

void RemoteMarket::apply(const protocol::ServerMessage& message) {
    if (const auto* welcome = std::get_if<protocol::Welcome>(&message)) {
        welcome_ = *welcome;
        started_ = welcome->started;
        now_ = welcome->time;
        market_.phase = welcome->phase;
        // A seat claimed again carries on with the balances and orders it had.
        ledger_ = Ledger{welcome->account.cash, welcome->account.position, welcome->account.fees};
        for (const OwnOrder& own : welcome->orders) {
            ledger_.recordRequest(NewOrder{.clientOrderId = own.clientOrderId,
                                           .side = own.side,
                                           .type = own.type,
                                           .price = own.price,
                                           .quantity = own.leaves});
            if (own.acknowledged) {
                ledger_.apply(
                    OrderAccepted{.clientOrderId = own.clientOrderId, .orderId = own.orderId});
            }
            if (own.cancelRequested) {
                ledger_.recordRequest(CancelOrder{.clientOrderId = own.clientOrderId});
            }
            nextId_ = std::max(nextId_, own.clientOrderId + 1);
        }
    } else if (const auto* start = std::get_if<protocol::Start>(&message)) {
        started_ = true;
        now_ = start->time;
    } else if (const auto* clock = std::get_if<protocol::Clock>(&message)) {
        now_ = clock->time;
    } else if (const auto* market = std::get_if<protocol::MarketMessage>(&message)) {
        now_ = market->time;
        ledger_.apply(market->event);
        if (const auto* rejected = std::get_if<OrderRejected>(&market->event)) {
            message_ = std::format("{} of order {} rejected: {}", toString(rejected->request),
                                   rejected->clientOrderId, toString(rejected->reason));
        } else if (const auto* trade = std::get_if<Trade>(&market->event)) {
            tape_.push_front({.time = market->time, .trade = *trade});
            if (tape_.size() > tapeLength_) {
                tape_.pop_back();
            }
            market_.lastTrade = trade->price;
            ++market_.trades;
            market_.volume += trade->quantity;
        } else if (const auto* top = std::get_if<TopOfBook>(&market->event)) {
            market_.bid = top->bid;
            market_.ask = top->ask;
        } else if (const auto* depth = std::get_if<BookDepth>(&market->event)) {
            market_.bids = depth->bids;
            market_.asks = depth->asks;
        } else if (const auto* phase = std::get_if<PhaseChanged>(&market->event)) {
            market_.phase = phase->phase;
            if (!isAuction(phase->phase)) {
                market_.indicative.reset();
            }
        } else if (const auto* indicative = std::get_if<Indicative>(&market->event)) {
            market_.indicative = indicative->uncross;
        }
    } else if (const auto* end = std::get_if<protocol::End>(&message)) {
        end_ = *end;
        now_ = end->time;
        message_ = std::format("the session is over: pnl {:+} before fees; press q", end->pnl);
    } else {
        const auto& error = std::get<protocol::Error>(message);
        message_ = std::format("server: {}{}", error.message, error.fatal ? " (disconnected)" : "");
    }
}

NewOrder RemoteMarket::order(Side side, OrderType type, Price price, Quantity quantity) {
    NewOrder order{.clientOrderId = nextId_++, .side = side, .type = type, .quantity = quantity};
    if (type == OrderType::Limit) {
        order.price = price;
    }
    ledger_.recordRequest(order);
    return order;
}

std::vector<CancelOrder> RemoteMarket::cancels(std::optional<Price> price) {
    std::vector<CancelOrder> sent;
    for (const auto& [id, own] : ledger_.orders()) {
        if (own.type == OrderType::Limit && !own.cancelRequested &&
            (!price || own.price == *price)) {
            sent.push_back({.clientOrderId = id});
        }
    }
    for (const CancelOrder& cancel : sent) {
        ledger_.recordRequest(cancel);
    }
    return sent;
}

} // namespace crowdbook

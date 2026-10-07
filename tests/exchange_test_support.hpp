#pragma once

#include <optional>
#include <ostream>

#include "crowdbook/exchange.hpp"
#include "crowdbook/messages.hpp"
#include "order_test_support.hpp"

// GoogleTest printers for exchange messages and accounts, found by argument-dependent lookup.
namespace crowdbook {

inline void PrintTo(const OrderAccepted& event, std::ostream* os) {
    *os << "Accepted{agent " << event.agent << ", client " << event.clientOrderId << ", order "
        << event.orderId << ", " << toString(event.side) << " " << toString(event.type) << " "
        << event.quantity << " @ " << event.price << "}";
}

inline void PrintTo(const OrderRejected& event, std::ostream* os) {
    *os << "Rejected{agent " << event.agent << ", client " << event.clientOrderId << ", "
        << toString(event.request) << ": " << toString(event.reason) << "}";
}

inline void PrintTo(const OrderModified& event, std::ostream* os) {
    *os << "Modified{agent " << event.agent << ", client " << event.clientOrderId << ", order "
        << event.orderId << ", now " << event.quantity << " @ " << event.price << "}";
}

inline void PrintTo(const OrderFilled& event, std::ostream* os) {
    *os << "Filled{agent " << event.agent << ", client " << event.clientOrderId << ", order "
        << event.orderId << ", " << toString(event.side) << " " << event.quantity << " @ "
        << event.price << ", leaves " << event.leavesQuantity << ", "
        << toString(event.liquidity) << "}";
}

inline void PrintTo(const OrderCancelled& event, std::ostream* os) {
    *os << "Cancelled{agent " << event.agent << ", client " << event.clientOrderId << ", order "
        << event.orderId << ", " << event.quantity << ", " << toString(event.reason) << "}";
}

inline void PrintTo(const Trade& event, std::ostream* os) {
    *os << "Trade{" << event.quantity << " @ " << event.price << ", aggressor "
        << toString(event.aggressorSide) << "}";
}

inline void PrintTo(const TopOfBook& event, std::ostream* os) {
    const auto printLevel = [os](const std::optional<LevelSummary>& level) {
        if (level) {
            PrintTo(*level, os);
        } else {
            *os << "none";
        }
    };
    *os << "TopOfBook{bid ";
    printLevel(event.bid);
    *os << ", ask ";
    printLevel(event.ask);
    *os << "}";
}

inline void PrintTo(const MarketSnapshot& market, std::ostream* os) {
    PrintTo(TopOfBook{.bid = market.bid, .ask = market.ask}, os);
    *os << " last trade ";
    if (market.lastTrade) {
        *os << *market.lastTrade;
    } else {
        *os << "none";
    }
}

inline void PrintTo(const Account& account, std::ostream* os) {
    *os << "Account{cash " << account.cash << ", position " << account.position << ", open buy "
        << account.openBuyQuantity << ", open sell " << account.openSellQuantity << "}";
}

} // namespace crowdbook

namespace crowdbook::test {

inline NewOrder limitOrder(ClientOrderId clientOrderId, Side side, Price price, Quantity quantity,
                           TimeInForce timeInForce = TimeInForce::GoodTillCancel) {
    return {.clientOrderId = clientOrderId,
            .side = side,
            .type = OrderType::Limit,
            .timeInForce = timeInForce,
            .price = price,
            .quantity = quantity};
}

inline NewOrder marketOrder(ClientOrderId clientOrderId, Side side, Quantity quantity) {
    return {.clientOrderId = clientOrderId,
            .side = side,
            .type = OrderType::Market,
            .quantity = quantity};
}

} // namespace crowdbook::test

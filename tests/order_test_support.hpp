#pragma once

#include <ostream>

#include "crowdbook/types.hpp"

// GoogleTest finds these printers by argument-dependent lookup, so failed assertions show
// readable values instead of raw bytes.
namespace crowdbook {

inline void PrintTo(Side side, std::ostream* os) { *os << toString(side); }

inline void PrintTo(const Fill& fill, std::ostream* os) {
    *os << "Fill{maker " << fill.makerOrderId << " (agent " << fill.makerOwner << "), taker "
        << fill.takerOrderId << " (agent " << fill.takerOwner << ", " << toString(fill.takerSide)
        << "), " << fill.quantity << " @ " << fill.price << ", maker left "
        << fill.makerRemaining << "}";
}

inline void PrintTo(const OrderResult& result, std::ostream* os) {
    *os << "OrderResult{" << toString(result.status) << ", filled " << result.filled
        << ", remaining " << result.remaining << ", cancel: " << toString(result.cancelReason)
        << ", reject: " << toString(result.rejectReason) << "}";
}

inline void PrintTo(const RestingOrder& order, std::ostream* os) {
    *os << "RestingOrder{id " << order.id << ", agent " << order.owner << ", "
        << toString(order.side) << " " << order.remaining << " @ " << order.price << "}";
}

inline void PrintTo(const LevelSummary& level, std::ostream* os) {
    *os << "Level{" << level.quantity << " @ " << level.price << " in " << level.orderCount
        << " orders}";
}

} // namespace crowdbook

namespace crowdbook::test {

inline OrderRequest limit(OrderId id, AgentId owner, Side side, Price price, Quantity quantity,
                          TimeInForce timeInForce = TimeInForce::GoodTillCancel) {
    return {.id = id,
            .owner = owner,
            .side = side,
            .type = OrderType::Limit,
            .timeInForce = timeInForce,
            .price = price,
            .quantity = quantity};
}

inline OrderRequest market(OrderId id, AgentId owner, Side side, Quantity quantity) {
    return {.id = id, .owner = owner, .side = side, .type = OrderType::Market, .quantity = quantity};
}

inline OrderResult resting(Quantity remaining, Quantity traded = 0) {
    return {.status = OrderStatus::Resting, .filled = traded, .remaining = remaining};
}

inline OrderResult filled(Quantity quantity) {
    return {.status = OrderStatus::Filled, .filled = quantity};
}

inline OrderResult cancelled(CancelReason reason, Quantity traded = 0) {
    return {.status = OrderStatus::Cancelled, .filled = traded, .cancelReason = reason};
}

inline OrderResult rejected(RejectReason reason) {
    return {.status = OrderStatus::Rejected, .rejectReason = reason};
}

} // namespace crowdbook::test

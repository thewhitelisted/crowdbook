#include "crowdbook/types.hpp"

namespace crowdbook {

std::string_view toString(Side side) noexcept {
    switch (side) {
    case Side::Buy:
        return "buy";
    case Side::Sell:
        return "sell";
    }
    return "unknown";
}

std::string_view toString(OrderType type) noexcept {
    switch (type) {
    case OrderType::Limit:
        return "limit";
    case OrderType::Market:
        return "market";
    }
    return "unknown";
}

std::string_view toString(TimeInForce timeInForce) noexcept {
    switch (timeInForce) {
    case TimeInForce::GoodTillCancel:
        return "good-till-cancel";
    case TimeInForce::ImmediateOrCancel:
        return "immediate-or-cancel";
    }
    return "unknown";
}

std::string_view toString(OrderStatus status) noexcept {
    switch (status) {
    case OrderStatus::Rejected:
        return "rejected";
    case OrderStatus::Resting:
        return "resting";
    case OrderStatus::Filled:
        return "filled";
    case OrderStatus::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

std::string_view toString(CancelReason reason) noexcept {
    switch (reason) {
    case CancelReason::None:
        return "none";
    case CancelReason::ImmediateOrCancel:
        return "immediate-or-cancel";
    case CancelReason::SelfTrade:
        return "self-trade";
    }
    return "unknown";
}

std::string_view toString(RejectReason reason) noexcept {
    switch (reason) {
    case RejectReason::None:
        return "none";
    case RejectReason::NonPositiveQuantity:
        return "non-positive quantity";
    case RejectReason::DuplicateOrderId:
        return "duplicate order id";
    case RejectReason::UnknownOrderId:
        return "unknown order id";
    }
    return "unknown";
}

} // namespace crowdbook

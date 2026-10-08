#include "crowdbook/types.hpp"

#include <format>
#include <string>

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
    case TimeInForce::PostOnly:
        return "post-only";
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
    case CancelReason::Requested:
        return "requested";
    case CancelReason::ImmediateOrCancel:
        return "immediate-or-cancel";
    case CancelReason::SelfTrade:
        return "self-trade";
    }
    return "unknown";
}

std::string formatFee(Fee fee) {
    const Fee magnitude = fee < 0 ? -fee : fee;
    std::string text = std::format("{}{}", fee < 0 ? "-" : "", magnitude / kFeeUnitsPerTickLot);
    if (const Fee fraction = magnitude % kFeeUnitsPerTickLot; fraction != 0) {
        std::string digits = std::format("{:03}", fraction);
        while (digits.back() == '0') {
            digits.pop_back();
        }
        text += '.' + digits;
    }
    return text;
}

std::string_view toString(RejectReason reason) noexcept {
    switch (reason) {
    case RejectReason::None:
        return "none";
    case RejectReason::NonPositiveQuantity:
        return "non-positive quantity";
    case RejectReason::InvalidPrice:
        return "invalid price";
    case RejectReason::OrderSizeLimit:
        return "order size limit";
    case RejectReason::PositionLimit:
        return "position limit";
    case RejectReason::DuplicateOrderId:
        return "duplicate order id";
    case RejectReason::DuplicateClientOrderId:
        return "duplicate client order id";
    case RejectReason::UnknownOrderId:
        return "unknown order id";
    case RejectReason::UnknownAgent:
        return "unknown agent";
    case RejectReason::PostOnlyWouldTrade:
        return "post-only would trade";
    case RejectReason::LossLimit:
        return "loss limit";
    case RejectReason::RateLimit:
        return "rate limit";
    }
    return "unknown";
}

} // namespace crowdbook

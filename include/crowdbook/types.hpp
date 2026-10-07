#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace crowdbook {

// Prices are whole numbers of ticks and quantities whole numbers of lots, so matching and
// accounting never touch floating point.
using Price = std::int64_t;
using Quantity = std::int64_t;
using OrderId = std::uint64_t;
using AgentId = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };

[[nodiscard]] constexpr Side opposite(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

enum class OrderType : std::uint8_t {
    Limit,  // trades at its limit price or better
    Market, // trades at any price and never rests
};

// What happens to the part of a limit order that does not trade on arrival.
enum class TimeInForce : std::uint8_t {
    GoodTillCancel,    // rests on the book until filled or cancelled
    ImmediateOrCancel, // is cancelled
};

struct OrderRequest {
    OrderId id = 0;
    AgentId owner = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce timeInForce = TimeInForce::GoodTillCancel; // ignored for market orders
    Price price = 0;                                       // ignored for market orders
    Quantity quantity = 0;
};

// One execution between a resting order (the maker) and an incoming order (the taker).
struct Fill {
    OrderId makerOrderId = 0;
    OrderId takerOrderId = 0;
    AgentId makerOwner = 0;
    AgentId takerOwner = 0;
    Side takerSide = Side::Buy;
    Price price = 0; // always the maker's price
    Quantity quantity = 0;
    Quantity makerRemaining = 0; // maker's open quantity after this fill; 0 means fully filled

    friend bool operator==(const Fill&, const Fill&) = default;
};

enum class OrderStatus : std::uint8_t {
    Rejected,  // the request was invalid and nothing changed
    Resting,   // open quantity is on the book
    Filled,    // fully filled on arrival
    Cancelled, // unfilled quantity was cancelled; see CancelReason
};

enum class CancelReason : std::uint8_t {
    None,
    ImmediateOrCancel, // immediate-or-cancel or market order that could not fill completely
    SelfTrade,         // reached a resting order from the same owner
};

enum class RejectReason : std::uint8_t {
    None,
    NonPositiveQuantity,
    DuplicateOrderId,
    UnknownOrderId,
};

struct OrderResult {
    OrderStatus status = OrderStatus::Rejected;
    Quantity filled = 0;    // quantity traded by this request
    Quantity remaining = 0; // open quantity left on the book
    CancelReason cancelReason = CancelReason::None;
    RejectReason rejectReason = RejectReason::None;

    friend bool operator==(const OrderResult&, const OrderResult&) = default;
};

// A resting order as seen from outside the book.
struct RestingOrder {
    OrderId id = 0;
    AgentId owner = 0;
    Side side = Side::Buy;
    Price price = 0;
    Quantity remaining = 0;

    friend bool operator==(const RestingOrder&, const RestingOrder&) = default;
};

// All resting orders at one price, aggregated.
struct LevelSummary {
    Price price = 0;
    Quantity quantity = 0;
    std::size_t orderCount = 0;

    friend bool operator==(const LevelSummary&, const LevelSummary&) = default;
};

[[nodiscard]] std::string_view toString(Side side) noexcept;
[[nodiscard]] std::string_view toString(OrderType type) noexcept;
[[nodiscard]] std::string_view toString(TimeInForce timeInForce) noexcept;
[[nodiscard]] std::string_view toString(OrderStatus status) noexcept;
[[nodiscard]] std::string_view toString(CancelReason reason) noexcept;
[[nodiscard]] std::string_view toString(RejectReason reason) noexcept;

} // namespace crowdbook

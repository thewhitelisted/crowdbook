#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

#include "crowdbook/types.hpp"

namespace crowdbook {

// Requests an agent sends to the exchange. Agents name their own orders with client order ids, so
// they can cancel or modify an order before its acknowledgement arrives, and can never name
// another agent's order.

struct NewOrder {
    ClientOrderId clientOrderId = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce timeInForce = TimeInForce::GoodTillCancel; // ignored for market orders
    Price price = 0;                                       // ignored for market orders
    Quantity quantity = 0;

    friend bool operator==(const NewOrder&, const NewOrder&) = default;
};

struct CancelOrder {
    ClientOrderId clientOrderId = 0;

    friend bool operator==(const CancelOrder&, const CancelOrder&) = default;
};

// Sets a live order's price and open quantity, with the queue rules of OrderBook::modify.
struct ModifyOrder {
    ClientOrderId clientOrderId = 0;
    Price price = 0;
    Quantity quantity = 0;

    friend bool operator==(const ModifyOrder&, const ModifyOrder&) = default;
};

using Request = std::variant<NewOrder, CancelOrder, ModifyOrder>;

enum class RequestKind : std::uint8_t { New, Cancel, Modify };

[[nodiscard]] RequestKind kindOf(const Request& request);
[[nodiscard]] ClientOrderId clientOrderIdOf(const Request& request);

// Private reports, each addressed to one agent.

struct OrderAccepted {
    AgentId agent = 0;
    ClientOrderId clientOrderId = 0;
    OrderId orderId = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce timeInForce = TimeInForce::GoodTillCancel;
    Price price = 0;
    Quantity quantity = 0;

    friend bool operator==(const OrderAccepted&, const OrderAccepted&) = default;
};

struct OrderRejected {
    AgentId agent = 0;
    ClientOrderId clientOrderId = 0;
    RequestKind request = RequestKind::New;
    RejectReason reason = RejectReason::None;

    friend bool operator==(const OrderRejected&, const OrderRejected&) = default;
};

struct OrderModified {
    AgentId agent = 0;
    ClientOrderId clientOrderId = 0;
    OrderId orderId = 0;
    Price price = 0;
    Quantity quantity = 0; // new open quantity

    friend bool operator==(const OrderModified&, const OrderModified&) = default;
};

enum class Liquidity : std::uint8_t {
    Maker, // the order was resting
    Taker, // the order was incoming
};

struct OrderFilled {
    AgentId agent = 0;
    ClientOrderId clientOrderId = 0;
    OrderId orderId = 0;
    Side side = Side::Buy;
    Price price = 0;
    Quantity quantity = 0;
    Quantity leavesQuantity = 0; // still open after this fill
    Liquidity liquidity = Liquidity::Maker;

    friend bool operator==(const OrderFilled&, const OrderFilled&) = default;
};

struct OrderCancelled {
    AgentId agent = 0;
    ClientOrderId clientOrderId = 0;
    OrderId orderId = 0;
    Quantity quantity = 0; // the quantity that was cancelled
    CancelReason reason = CancelReason::Requested;

    friend bool operator==(const OrderCancelled&, const OrderCancelled&) = default;
};

// Public market data, the same for everyone. It never identifies agents or orders.

struct Trade {
    Price price = 0;
    Quantity quantity = 0;
    Side aggressorSide = Side::Buy;

    friend bool operator==(const Trade&, const Trade&) = default;
};

struct TopOfBook {
    std::optional<LevelSummary> bid{};
    std::optional<LevelSummary> ask{};

    friend bool operator==(const TopOfBook&, const TopOfBook&) = default;
};

// The public market as one agent can see it: the best bid and ask and the last trade price, as
// the exchange had published them one of the agent's latencies ago.
struct MarketSnapshot {
    std::optional<LevelSummary> bid{};
    std::optional<LevelSummary> ask{};
    std::optional<Price> lastTrade{};

    friend bool operator==(const MarketSnapshot&, const MarketSnapshot&) = default;
};

using Event = std::variant<OrderAccepted, OrderRejected, OrderModified, OrderFilled,
                           OrderCancelled, Trade, TopOfBook>;

// The agent a private event is addressed to, or nullopt for public market data.
[[nodiscard]] std::optional<AgentId> recipient(const Event& event);

[[nodiscard]] std::string_view toString(RequestKind kind) noexcept;
[[nodiscard]] std::string_view toString(Liquidity liquidity) noexcept;

} // namespace crowdbook

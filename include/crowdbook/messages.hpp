#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

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
    // The sender's own id for a larger order that this one is part of, or 0. The exchange ignores
    // it; the event log keeps it, so that an analysis can put a large order's pieces together.
    std::uint64_t parent = 0;

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
    Maker,   // the order was resting
    Taker,   // the order was incoming
    Auction, // the order traded when an auction uncrossed
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
    Fee fee = 0; // charged for this fill, in fee units; negative for a rebate

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
    // The incoming order's side; for a trade in an auction's uncross, which has none, the side
    // of the newer of the two orders.
    Side aggressorSide = Side::Buy;
    bool auction = false; // part of an auction's uncross

    friend bool operator==(const Trade&, const Trade&) = default;
};

// The market has moved to another phase of its trading day. Leaving an auction, `price` is the
// price it uncrossed at, if anything traded.
struct PhaseChanged {
    Phase phase = Phase::Continuous;
    std::optional<Price> price{};

    friend bool operator==(const PhaseChanged&, const PhaseChanged&) = default;
};

// During an auction: where the book would uncross now, or nothing if nothing would trade.
// Published whenever it changes.
struct Indicative {
    std::optional<Uncross> uncross{};

    friend bool operator==(const Indicative&, const Indicative&) = default;
};

struct TopOfBook {
    std::optional<LevelSummary> bid{};
    std::optional<LevelSummary> ask{};

    friend bool operator==(const TopOfBook&, const TopOfBook&) = default;
};

// The best price levels on each side, best first, as deep as the exchange's depth feed goes.
// Published only by an exchange configured with a depth feed.
struct BookDepth {
    std::vector<LevelSummary> bids{};
    std::vector<LevelSummary> asks{};

    friend bool operator==(const BookDepth&, const BookDepth&) = default;
};

// The public market as one agent can see it: the best bid and ask, the last trade price, the
// trades and volume so far and, with a depth feed, the best levels on each side, as the exchange
// had published them one of the agent's latencies ago.
struct MarketSnapshot {
    std::optional<LevelSummary> bid{};
    std::optional<LevelSummary> ask{};
    std::optional<Price> lastTrade{};
    std::vector<LevelSummary> bids{}; // best first; empty without a depth feed
    std::vector<LevelSummary> asks{};
    std::uint64_t trades = 0; // trades published since the start of the run
    Quantity volume = 0;      // and the lots they traded
    Phase phase = Phase::Continuous;
    std::optional<Uncross> indicative{}; // during an auction, where it would uncross

    friend bool operator==(const MarketSnapshot&, const MarketSnapshot&) = default;
};

using Event = std::variant<OrderAccepted, OrderRejected, OrderModified, OrderFilled,
                           OrderCancelled, Trade, TopOfBook, BookDepth, PhaseChanged,
                           Indicative>;

// The agent a private event is addressed to, or nullopt for public market data.
[[nodiscard]] std::optional<AgentId> recipient(const Event& event);

[[nodiscard]] std::string_view toString(RequestKind kind) noexcept;
[[nodiscard]] std::string_view toString(Liquidity liquidity) noexcept;

} // namespace crowdbook

#pragma once

#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "crowdbook/types.hpp"

namespace crowdbook {

// Limit order book for one instrument, matching with price-time priority.
//
// The book does not assign order ids or decide who may cancel what; the exchange does both before
// calling it. Executions are appended to a caller-owned vector so a hot loop can reuse one buffer.
class OrderBook {
public:
    static constexpr std::size_t kAllLevels = std::numeric_limits<std::size_t>::max();

    OrderBook() = default;
    // Level lists point at nodes owned by this book, so a copy would point into the original.
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = default;
    OrderBook& operator=(OrderBook&&) = default;
    ~OrderBook() = default;

    // Matches the request against the opposite side, then rests whatever is left of a
    // good-till-cancel limit order. Executions are appended to `fills`.
    OrderResult submit(const OrderRequest& request, std::vector<Fill>& fills);

    // Sets a resting order's price and open quantity. Lowering the quantity at the same price keeps
    // the order's place in the queue; any other change re-enters it at the back of its new price
    // level, where it may trade immediately. Executions are appended to `fills`.
    OrderResult modify(OrderId id, Price price, Quantity quantity, std::vector<Fill>& fills);

    // Removes a resting order and returns it as it was, or nullopt if no such order is resting.
    std::optional<RestingOrder> cancel(OrderId id);

    [[nodiscard]] std::optional<RestingOrder> find(OrderId id) const;
    [[nodiscard]] std::optional<Price> bestBid() const noexcept;
    [[nodiscard]] std::optional<Price> bestAsk() const noexcept;
    // Price levels on one side, best price first.
    [[nodiscard]] std::vector<LevelSummary> depth(Side side,
                                                  std::size_t maxLevels = kAllLevels) const;
    [[nodiscard]] std::size_t orderCount() const noexcept;

    // Checks the book's internal structure and describes the first problem found, or returns
    // nullopt if it is consistent. Walks every order, so it is meant for tests.
    [[nodiscard]] std::optional<std::string> audit() const;

private:
    struct Level;

    // A resting order, linked into the FIFO queue of its price level.
    struct Node {
        OrderId id = 0;
        AgentId owner = 0;
        Side side = Side::Buy;
        Price price = 0;
        Quantity remaining = 0;
        Level* level = nullptr;
        Node* prev = nullptr;
        Node* next = nullptr;
    };

    struct Level {
        Price price = 0;
        Quantity quantity = 0; // sum of the open quantity of its orders
        std::size_t orderCount = 0;
        Node* head = nullptr; // oldest order, next to trade
        Node* tail = nullptr;
    };

    struct MatchOutcome {
        Quantity filled = 0;
        bool selfTrade = false;
    };

    // Ordered so that begin() is always the best price.
    using Bids = std::map<Price, Level, std::greater<>>;
    using Asks = std::map<Price, Level, std::less<>>;

    template <typename Levels>
    MatchOutcome match(Levels& levels, const OrderRequest& taker, std::vector<Fill>& fills);
    template <typename Levels>
    void rest(Levels& levels, Node& node);
    void unlink(Node& node);
    static RestingOrder toRestingOrder(const Node& node) noexcept;

    Bids bids_;
    Asks asks_;
    // Owns every resting order. Nodes never move once inserted, so level queues can link them.
    std::unordered_map<OrderId, Node> orders_;
};

} // namespace crowdbook

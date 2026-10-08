#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "crowdbook/types.hpp"

namespace crowdbook {

// The single price an auction's book trades at when it uncrosses, and what trades there.
struct Uncross {
    Price price = 0;
    Quantity volume = 0; // lots that trade
    // Lots bid at the price or better less lots offered at the price or better: what is left
    // over on the buying side when positive, on the selling side when negative.
    Quantity imbalance = 0;

    friend bool operator==(const Uncross&, const Uncross&) = default;
};

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
    // good-till-cancel or post-only limit order. Executions are appended to `fills`. The exchange
    // rejects a post-only order that would trade before it reaches the book.
    OrderResult submit(const OrderRequest& request, std::vector<Fill>& fills);

    // Sets a resting order's price and open quantity. Lowering the quantity at the same price keeps
    // the order's place in the queue; any other change re-enters it at the back of its new price
    // level, where it may trade immediately. Executions are appended to `fills`.
    OrderResult modify(OrderId id, Price price, Quantity quantity, std::vector<Fill>& fills);

    // Removes a resting order and returns it as it was, or nullopt if no such order is resting.
    std::optional<RestingOrder> cancel(OrderId id);

    // Turns matching off for a call auction, or back on. While it is off, limit orders and
    // modifies rest without trading, so the book may cross; market and immediate-or-cancel
    // orders are cancelled unfilled.
    void setMatching(bool matching) noexcept { matching_ = matching; }
    [[nodiscard]] bool matching() const noexcept { return matching_; }

    // The price the book would uncross at now, or nullopt if nothing would trade: the price that
    // trades the most lots, then the one with the smallest imbalance, then the one nearest
    // `reference`, then the lowest.
    [[nodiscard]] std::optional<Uncross> indicative(Price reference) const;
    // Trades every order the indicative price reaches, at that price: bids from the highest and
    // asks from the lowest, oldest first within a price. The older order of each pair is the
    // maker in the fills appended to `fills`. When a pair would trade an owner with itself, the
    // newer order is taken off the book and appended to `selfTrades` instead, and the price stays
    // what it was. Returns the uncross, or nullopt if nothing would trade. Without self-trades the
    // book is never crossed afterwards: a bid and an ask left crossing would have made another
    // price trade more. Cancelling a self-trade takes lots out of the volume the price was chosen
    // for, which can leave the book crossed; uncross again until a round cancels none.
    std::optional<Uncross> uncross(Price reference, std::vector<Fill>& fills,
                                   std::vector<RestingOrder>& selfTrades);

    [[nodiscard]] std::optional<RestingOrder> find(OrderId id) const;
    [[nodiscard]] std::optional<Price> bestBid() const noexcept;
    [[nodiscard]] std::optional<Price> bestAsk() const noexcept;
    // The best price level on one side, or nullopt if that side is empty.
    [[nodiscard]] std::optional<LevelSummary> bestLevel(Side side) const noexcept;
    // Price levels on one side, best price first.
    [[nodiscard]] std::vector<LevelSummary> depth(Side side,
                                                  std::size_t maxLevels = kAllLevels) const;
    // The same, into `levels`, which is cleared first, so a caller can reuse one buffer.
    void depth(Side side, std::size_t maxLevels, std::vector<LevelSummary>& levels) const;
    [[nodiscard]] std::size_t orderCount() const noexcept;

    // Checks the book's internal structure and describes the first problem found, or returns
    // nullopt if it is consistent. A crossed book is a problem only while matching is on. Walks
    // every order, so it is meant for tests.
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
        std::uint64_t sequence = 0; // when it was last placed on the book, for time priority
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

    void erase(Node& node);

    Bids bids_;
    Asks asks_;
    bool matching_ = true;
    std::uint64_t nextSequence_ = 0;
    // Owns every resting order. Nodes never move once inserted, so level queues can link them.
    // Only looked up: the levels, not this map, give the order in which orders trade.
    std::unordered_map<OrderId, Node> orders_;
};

} // namespace crowdbook

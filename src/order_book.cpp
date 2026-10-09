#include "crowdbook/order_book.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <type_traits>
#include <utility>

namespace crowdbook {

namespace {

OrderResult rejected(RejectReason reason) {
    return {.status = OrderStatus::Rejected, .rejectReason = reason};
}

// Whether an incoming limit order on `takerSide` accepts a resting price.
bool withinLimit(Side takerSide, Price limit, Price restingPrice) {
    return takerSide == Side::Buy ? restingPrice <= limit : restingPrice >= limit;
}

} // namespace

OrderResult OrderBook::submit(const OrderRequest& request, std::vector<Fill>& fills) {
    if (request.quantity <= 0) {
        return rejected(RejectReason::NonPositiveQuantity);
    }
    if (orders_.contains(request.id)) {
        return rejected(RejectReason::DuplicateOrderId);
    }
    const bool mayRest = request.type == OrderType::Limit &&
                         request.timeInForce != TimeInForce::ImmediateOrCancel;
    if (mayRest) {
        prepareToRest(request.side);
    }

    const MatchOutcome outcome = !matching_                ? MatchOutcome{}
                                 : request.side == Side::Buy ? match(asks_, request, fills)
                                                             : match(bids_, request, fills);
    const Quantity remaining = request.quantity - outcome.filled;

    if (remaining == 0) {
        return {.status = OrderStatus::Filled, .filled = outcome.filled};
    }
    if (outcome.selfTrade) {
        return {.status = OrderStatus::Cancelled,
                .filled = outcome.filled,
                .cancelReason = CancelReason::SelfTrade};
    }
    if (request.type == OrderType::Market ||
        request.timeInForce == TimeInForce::ImmediateOrCancel) {
        return {.status = OrderStatus::Cancelled,
                .filled = outcome.filled,
                .cancelReason = CancelReason::ImmediateOrCancel};
    }

    Node& node = newNode();
    node = Node{.id = request.id,
                .owner = request.owner,
                .side = request.side,
                .price = request.price,
                .remaining = remaining};
    orders_.tryEmplace(request.id, &node);
    if (request.side == Side::Buy) {
        rest(bids_, node);
    } else {
        rest(asks_, node);
    }
    return {.status = OrderStatus::Resting, .filled = outcome.filled, .remaining = remaining};
}

OrderResult OrderBook::modify(OrderId id, Price price, Quantity quantity,
                              std::vector<Fill>& fills) {
    Node* const* found = orders_.find(id);
    if (found == nullptr) {
        return rejected(RejectReason::UnknownOrderId);
    }
    if (quantity <= 0) {
        return rejected(RejectReason::NonPositiveQuantity);
    }

    Node& node = **found;
    if (price == node.price && quantity <= node.remaining) {
        node.level->quantity -= node.remaining - quantity;
        changed(node.side);
        node.remaining = quantity;
        return {.status = OrderStatus::Resting, .remaining = quantity};
    }

    // Any other change loses queue position: cancel and re-enter as a new order with the same id,
    // with room made first, so that the order is not lost if memory runs out.
    prepareToRest(node.side);
    const OrderRequest replacement{.id = id,
                                   .owner = node.owner,
                                   .side = node.side,
                                   .type = OrderType::Limit,
                                   .timeInForce = TimeInForce::GoodTillCancel,
                                   .price = price,
                                   .quantity = quantity};
    cancel(id);
    return submit(replacement, fills);
}

std::optional<RestingOrder> OrderBook::cancel(OrderId id) {
    Node* const* found = orders_.find(id);
    if (found == nullptr) {
        return std::nullopt;
    }
    Node& node = **found;
    const RestingOrder cancelled = toRestingOrder(node);
    erase(node);
    return cancelled;
}

// Takes an order off its level, and the level off the book if it empties, and forgets the order.
void OrderBook::erase(Node& node) {
    const Level& level = *node.level;
    const Side side = node.side;
    const OrderId id = node.id;
    unlink(node);
    if (level.orderCount == 0) {
        const Price price = level.price;
        if (side == Side::Buy) {
            retire(bids_, bids_.find(price), spareBids_);
        } else {
            retire(asks_, asks_.find(price), spareAsks_);
        }
    }
    orders_.erase(id);
    freeNodes_.push_back(&node);
}

std::optional<Uncross> OrderBook::indicative(Price reference) const {
    // Every price that is some order's limit is a candidate, lowest first. Demand at a price is
    // what is bid at it or higher, supply what is offered at it or lower.
    Quantity demand = 0;
    for (const auto& [price, level] : bids_) {
        demand += level.quantity;
    }
    Quantity supply = 0;
    auto bid = bids_.rbegin(); // lowest bid first
    auto ask = asks_.begin();  // lowest ask first
    std::optional<Uncross> best;
    const auto better = [reference](const Uncross& a, const Uncross& b) {
        if (a.volume != b.volume) {
            return a.volume > b.volume;
        }
        if (std::abs(a.imbalance) != std::abs(b.imbalance)) {
            return std::abs(a.imbalance) < std::abs(b.imbalance);
        }
        if (std::abs(a.price - reference) != std::abs(b.price - reference)) {
            return std::abs(a.price - reference) < std::abs(b.price - reference);
        }
        return a.price < b.price;
    };
    while (bid != bids_.rend() || ask != asks_.end()) {
        const Price price = bid == bids_.rend()   ? ask->first
                            : ask == asks_.end() ? bid->first
                                                 : std::min(bid->first, ask->first);
        // Asks at this price now count toward supply; bids below it no longer count to demand.
        while (ask != asks_.end() && ask->first == price) {
            supply += ask->second.quantity;
            ++ask;
        }
        const Uncross here{.price = price,
                           .volume = std::min(demand, supply),
                           .imbalance = demand - supply};
        if (here.volume > 0 && (!best || better(here, *best))) {
            best = here;
        }
        while (bid != bids_.rend() && bid->first == price) {
            demand -= bid->second.quantity;
            ++bid;
        }
    }
    return best;
}

std::optional<Uncross> OrderBook::uncross(Price reference, std::vector<Fill>& fills,
                                          std::vector<RestingOrder>& selfTrades) {
    const std::optional<Uncross> result = indicative(reference);
    if (!result) {
        return std::nullopt;
    }
    const Price price = result->price;
    while (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= price &&
           asks_.begin()->first <= price) {
        Node& bid = *bids_.begin()->second.head;
        Node& ask = *asks_.begin()->second.head;
        const bool bidIsOlder = bid.sequence < ask.sequence;
        Node& maker = bidIsOlder ? bid : ask;
        Node& taker = bidIsOlder ? ask : bid;
        if (bid.owner == ask.owner) {
            selfTrades.push_back(toRestingOrder(taker));
            erase(taker);
            continue;
        }
        const Quantity quantity = std::min(bid.remaining, ask.remaining);
        // The fill first: if appending it fails, nothing has changed yet.
        fills.push_back({.makerOrderId = maker.id,
                         .takerOrderId = taker.id,
                         .makerOwner = maker.owner,
                         .takerOwner = taker.owner,
                         .takerSide = taker.side,
                         .price = price,
                         .quantity = quantity,
                         .makerRemaining = maker.remaining - quantity});
        maker.remaining -= quantity;
        taker.remaining -= quantity;
        maker.level->quantity -= quantity;
        taker.level->quantity -= quantity;
        changed(Side::Buy);
        changed(Side::Sell);
        if (bid.remaining == 0) {
            erase(bid);
        }
        if (ask.remaining == 0) {
            erase(ask);
        }
    }
    return result;
}

std::optional<RestingOrder> OrderBook::find(OrderId id) const {
    Node* const* found = orders_.find(id);
    if (found == nullptr) {
        return std::nullopt;
    }
    return toRestingOrder(**found);
}

std::optional<Price> OrderBook::bestBid() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

std::optional<LevelSummary> OrderBook::bestLevel(Side side) const noexcept {
    const auto summarize = [](const auto& levels) -> std::optional<LevelSummary> {
        if (levels.empty()) {
            return std::nullopt;
        }
        const Level& level = levels.begin()->second;
        return LevelSummary{
            .price = level.price, .quantity = level.quantity, .orderCount = level.orderCount};
    };
    return side == Side::Buy ? summarize(bids_) : summarize(asks_);
}

std::vector<LevelSummary> OrderBook::depth(Side side, std::size_t maxLevels) const {
    std::vector<LevelSummary> levels;
    depth(side, maxLevels, levels);
    return levels;
}

void OrderBook::depth(Side side, std::size_t maxLevels, std::vector<LevelSummary>& levels) const {
    levels.clear();
    const auto collect = [&](const auto& book) {
        levels.reserve(std::min(maxLevels, book.size()));
        for (const auto& [price, level] : book) {
            if (levels.size() == maxLevels) {
                break;
            }
            levels.push_back(
                {.price = price, .quantity = level.quantity, .orderCount = level.orderCount});
        }
    };
    if (side == Side::Buy) {
        collect(bids_);
    } else {
        collect(asks_);
    }
}

std::size_t OrderBook::orderCount() const noexcept { return orders_.size(); }

std::optional<std::string> OrderBook::audit() const {
    if (const auto bid = bestBid(), ask = bestAsk(); matching_ && bid && ask && *bid >= *ask) {
        return std::format("book is crossed: best bid {} >= best ask {}", *bid, *ask);
    }

    std::size_t linked = 0;
    const auto auditSide = [&](const auto& levels, Side side) -> std::optional<std::string> {
        for (const auto& [price, level] : levels) {
            if (level.price != price) {
                return std::format("level keyed {} has price {}", price, level.price);
            }
            if (level.head == nullptr) {
                return std::format("level {} is empty", price);
            }
            Quantity quantity = 0;
            std::size_t count = 0;
            const Node* previous = nullptr;
            for (const Node* node = level.head; node != nullptr; node = node->next) {
                if (++count > orders_.size()) {
                    return std::format("level {} queue does not terminate", price);
                }
                Node* const* indexed = orders_.find(node->id);
                if (indexed == nullptr || *indexed != node) {
                    return std::format("order {} is queued but not indexed", node->id);
                }
                if (node->prev != previous) {
                    return std::format("order {} has a broken back link", node->id);
                }
                if (node->level != &level || node->price != price || node->side != side) {
                    return std::format("order {} is queued under the wrong level", node->id);
                }
                if (node->remaining <= 0) {
                    return std::format("order {} has no open quantity", node->id);
                }
                quantity += node->remaining;
                previous = node;
            }
            if (level.tail != previous) {
                return std::format("level {} has the wrong tail", price);
            }
            if (level.quantity != quantity || level.orderCount != count) {
                return std::format("level {} records {} lots in {} orders, but holds {} in {}",
                                   price, level.quantity, level.orderCount, quantity, count);
            }
            linked += count;
        }
        return std::nullopt;
    };

    if (auto problem = auditSide(bids_, Side::Buy)) {
        return problem;
    }
    if (auto problem = auditSide(asks_, Side::Sell)) {
        return problem;
    }
    if (linked != orders_.size()) {
        return std::format("{} orders are indexed but {} are queued", orders_.size(), linked);
    }
    return std::nullopt;
}

template <typename Levels>
OrderBook::MatchOutcome OrderBook::match(Levels& levels, const OrderRequest& taker,
                                         std::vector<Fill>& fills) {
    const bool hasLimit = taker.type == OrderType::Limit;
    MatchOutcome outcome;

    while (outcome.filled < taker.quantity && !levels.empty()) {
        const auto best = levels.begin();
        Level& level = best->second;
        if (hasLimit && !withinLimit(taker.side, taker.price, level.price)) {
            break;
        }

        while (level.head != nullptr && outcome.filled < taker.quantity) {
            Node& maker = *level.head;
            if (maker.owner == taker.owner) {
                outcome.selfTrade = true;
                return outcome;
            }

            const Quantity quantity = std::min(taker.quantity - outcome.filled, maker.remaining);
            // The fill first: if appending it fails, nothing has changed yet.
            fills.push_back({.makerOrderId = maker.id,
                             .takerOrderId = taker.id,
                             .makerOwner = maker.owner,
                             .takerOwner = taker.owner,
                             .takerSide = taker.side,
                             .price = level.price,
                             .quantity = quantity,
                             .makerRemaining = maker.remaining - quantity});
            maker.remaining -= quantity;
            level.quantity -= quantity;
            changed(maker.side);
            outcome.filled += quantity;

            if (maker.remaining == 0) {
                unlink(maker);
                orders_.erase(maker.id);
                freeNodes_.push_back(&maker);
            }
        }

        if (level.head == nullptr) {
            if constexpr (std::is_same_v<Levels, Bids>) {
                retire(levels, best, spareBids_);
            } else {
                retire(levels, best, spareAsks_);
            }
        }
    }
    return outcome;
}

template <typename Levels>
void OrderBook::rest(Levels& levels, Node& node) {
    auto at = levels.lower_bound(node.price);
    if (at == levels.end() || at->first != node.price) {
        auto& spares = [this]() -> auto& {
            if constexpr (std::is_same_v<Levels, Bids>) {
                return spareBids_;
            } else {
                return spareAsks_;
            }
        }();
        // prepareToRest left a spare, so a new level allocates nothing.
        auto spare = std::move(spares.back());
        spares.pop_back();
        spare.key() = node.price;
        spare.mapped() = Level{.price = node.price};
        at = levels.insert(at, std::move(spare));
    }
    Level& level = at->second;
    node.level = &level;
    node.sequence = nextSequence_++;
    node.prev = level.tail;
    node.next = nullptr;
    if (level.tail != nullptr) {
        level.tail->next = &node;
    } else {
        level.head = &node;
    }
    level.tail = &node;
    level.quantity += node.remaining;
    changed(node.side);
    ++level.orderCount;
}

void OrderBook::unlink(Node& node) {
    Level& level = *node.level;
    if (node.prev != nullptr) {
        node.prev->next = node.next;
    } else {
        level.head = node.next;
    }
    if (node.next != nullptr) {
        node.next->prev = node.prev;
    } else {
        level.tail = node.prev;
    }
    level.quantity -= node.remaining;
    --level.orderCount;
    changed(node.side);
    node.level = nullptr;
    node.prev = nullptr;
    node.next = nullptr;
}

void OrderBook::makeRoomToRest(Side side) {
    orders_.reserve(orders_.size() + 1);
    if (freeNodes_.empty()) {
        if (freeNodes_.capacity() < nodes_.size() + 1) {
            freeNodes_.reserve(std::max<std::size_t>(16, 2 * (nodes_.size() + 1)));
        }
        freeNodes_.push_back(&nodes_.emplace_back());
    }
    if (side == Side::Buy) {
        prepareLevel<Bids>(spareBids_);
    } else {
        prepareLevel<Asks>(spareAsks_);
    }
}

template <typename Levels, typename Spares>
void OrderBook::prepareLevel(Spares& spares) {
    if (spares.capacity() < kSpareLevels) {
        spares.reserve(kSpareLevels);
    }
    if (spares.empty()) {
        Levels scratch;
        scratch.try_emplace(0);
        spares.push_back(scratch.extract(scratch.begin()));
    }
}

template <typename Levels, typename Spares>
void OrderBook::retire(Levels& levels, typename Levels::iterator level, Spares& spares) {
    if (spares.size() < spares.capacity()) {
        spares.push_back(levels.extract(level)); // within capacity, so it cannot allocate
    } else {
        levels.erase(level);
    }
}

OrderBook::Node& OrderBook::newNode() {
    // prepareToRest left a free node.
    Node& node = *freeNodes_.back();
    freeNodes_.pop_back();
    return node;
}

RestingOrder OrderBook::toRestingOrder(const Node& node) noexcept {
    return {.id = node.id,
            .owner = node.owner,
            .side = node.side,
            .price = node.price,
            .remaining = node.remaining};
}

} // namespace crowdbook

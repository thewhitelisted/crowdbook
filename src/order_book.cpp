#include "crowdbook/order_book.hpp"

#include <algorithm>
#include <format>

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

    const MatchOutcome outcome = request.side == Side::Buy ? match(asks_, request, fills)
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

    Node& node = orders_
                     .try_emplace(request.id, Node{.id = request.id,
                                                   .owner = request.owner,
                                                   .side = request.side,
                                                   .price = request.price,
                                                   .remaining = remaining})
                     .first->second;
    if (request.side == Side::Buy) {
        rest(bids_, node);
    } else {
        rest(asks_, node);
    }
    return {.status = OrderStatus::Resting, .filled = outcome.filled, .remaining = remaining};
}

OrderResult OrderBook::modify(OrderId id, Price price, Quantity quantity,
                              std::vector<Fill>& fills) {
    const auto it = orders_.find(id);
    if (it == orders_.end()) {
        return rejected(RejectReason::UnknownOrderId);
    }
    if (quantity <= 0) {
        return rejected(RejectReason::NonPositiveQuantity);
    }

    Node& node = it->second;
    if (price == node.price && quantity <= node.remaining) {
        node.level->quantity -= node.remaining - quantity;
        node.remaining = quantity;
        return {.status = OrderStatus::Resting, .remaining = quantity};
    }

    // Any other change loses queue position: cancel and re-enter as a new order with the same id.
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
    const auto it = orders_.find(id);
    if (it == orders_.end()) {
        return std::nullopt;
    }

    Node& node = it->second;
    const RestingOrder cancelled = toRestingOrder(node);
    const Level& level = *node.level;
    unlink(node);
    if (level.orderCount == 0) {
        const Price price = level.price;
        if (cancelled.side == Side::Buy) {
            bids_.erase(price);
        } else {
            asks_.erase(price);
        }
    }
    orders_.erase(it);
    return cancelled;
}

std::optional<RestingOrder> OrderBook::find(OrderId id) const {
    const auto it = orders_.find(id);
    if (it == orders_.end()) {
        return std::nullopt;
    }
    return toRestingOrder(it->second);
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

std::vector<LevelSummary> OrderBook::depth(Side side, std::size_t maxLevels) const {
    std::vector<LevelSummary> summary;
    const auto collect = [&](const auto& levels) {
        summary.reserve(std::min(maxLevels, levels.size()));
        for (const auto& [price, level] : levels) {
            if (summary.size() == maxLevels) {
                break;
            }
            summary.push_back(
                {.price = price, .quantity = level.quantity, .orderCount = level.orderCount});
        }
    };
    if (side == Side::Buy) {
        collect(bids_);
    } else {
        collect(asks_);
    }
    return summary;
}

std::size_t OrderBook::orderCount() const noexcept { return orders_.size(); }

std::optional<std::string> OrderBook::audit() const {
    if (const auto bid = bestBid(), ask = bestAsk(); bid && ask && *bid >= *ask) {
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
                const auto indexed = orders_.find(node->id);
                if (indexed == orders_.end() || &indexed->second != node) {
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
            maker.remaining -= quantity;
            level.quantity -= quantity;
            outcome.filled += quantity;
            fills.push_back({.makerOrderId = maker.id,
                             .takerOrderId = taker.id,
                             .makerOwner = maker.owner,
                             .takerOwner = taker.owner,
                             .takerSide = taker.side,
                             .price = level.price,
                             .quantity = quantity,
                             .makerRemaining = maker.remaining});

            if (maker.remaining == 0) {
                const OrderId filledId = maker.id;
                unlink(maker);
                orders_.erase(filledId);
            }
        }

        if (level.head == nullptr) {
            levels.erase(best);
        }
    }
    return outcome;
}

template <typename Levels>
void OrderBook::rest(Levels& levels, Node& node) {
    Level& level = levels.try_emplace(node.price, Level{.price = node.price}).first->second;
    node.level = &level;
    node.prev = level.tail;
    node.next = nullptr;
    if (level.tail != nullptr) {
        level.tail->next = &node;
    } else {
        level.head = &node;
    }
    level.tail = &node;
    level.quantity += node.remaining;
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
    node.level = nullptr;
    node.prev = nullptr;
    node.next = nullptr;
}

RestingOrder OrderBook::toRestingOrder(const Node& node) noexcept {
    return {.id = node.id,
            .owner = node.owner,
            .side = node.side,
            .price = node.price,
            .remaining = node.remaining};
}

} // namespace crowdbook

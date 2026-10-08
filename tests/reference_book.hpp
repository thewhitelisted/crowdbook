#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

#include "crowdbook/order_book.hpp"
#include "crowdbook/types.hpp"
#include "order_test_support.hpp"

namespace crowdbook::test {

// A deliberately naive order book that serves as the oracle for OrderBook. Orders sit in one flat
// list and every operation scans it, so the matching rules can be checked by reading this file.
class ReferenceBook {
public:
    OrderResult submit(const OrderRequest& request, std::vector<Fill>& fills) {
        if (request.quantity <= 0) {
            return rejected(RejectReason::NonPositiveQuantity);
        }
        if (findEntry(request.id) != entries_.end()) {
            return rejected(RejectReason::DuplicateOrderId);
        }

        Quantity remaining = request.quantity;
        while (matching && remaining > 0) {
            const auto maker = bestOpposite(request.side);
            if (maker == entries_.end()) {
                break;
            }
            if (request.type == OrderType::Limit && !accepts(request, maker->order.price)) {
                break;
            }
            if (maker->order.owner == request.owner) {
                return cancelled(CancelReason::SelfTrade, request.quantity - remaining);
            }

            const Quantity quantity = std::min(remaining, maker->order.remaining);
            maker->order.remaining -= quantity;
            remaining -= quantity;
            fills.push_back({.makerOrderId = maker->order.id,
                             .takerOrderId = request.id,
                             .makerOwner = maker->order.owner,
                             .takerOwner = request.owner,
                             .takerSide = request.side,
                             .price = maker->order.price,
                             .quantity = quantity,
                             .makerRemaining = maker->order.remaining});
            if (maker->order.remaining == 0) {
                entries_.erase(maker);
            }
        }

        const Quantity traded = request.quantity - remaining;
        if (remaining == 0) {
            return filled(traded);
        }
        if (request.type == OrderType::Market ||
            request.timeInForce == TimeInForce::ImmediateOrCancel) {
            return cancelled(CancelReason::ImmediateOrCancel, traded);
        }
        entries_.push_back({.order = {.id = request.id,
                                      .owner = request.owner,
                                      .side = request.side,
                                      .price = request.price,
                                      .remaining = remaining},
                            .sequence = nextSequence_++});
        return resting(remaining, traded);
    }

    OrderResult modify(OrderId id, Price price, Quantity quantity, std::vector<Fill>& fills) {
        const auto entry = findEntry(id);
        if (entry == entries_.end()) {
            return rejected(RejectReason::UnknownOrderId);
        }
        if (quantity <= 0) {
            return rejected(RejectReason::NonPositiveQuantity);
        }
        if (price == entry->order.price && quantity <= entry->order.remaining) {
            entry->order.remaining = quantity;
            return resting(quantity);
        }
        const RestingOrder original = entry->order;
        entries_.erase(entry);
        return submit(limit(id, original.owner, original.side, price, quantity), fills);
    }

    // Off for a call auction: orders rest without trading.
    bool matching = true;

    // Tries every price any order is at, and keeps the one that trades the most lots, then has
    // the smallest imbalance, then is nearest the reference, then is lowest.
    [[nodiscard]] std::optional<Uncross> indicative(Price reference) const {
        std::optional<Uncross> best;
        for (const Entry& candidate : entries_) {
            const Price price = candidate.order.price;
            Quantity demand = 0;
            Quantity supply = 0;
            for (const Entry& entry : entries_) {
                if (entry.order.side == Side::Buy && entry.order.price >= price) {
                    demand += entry.order.remaining;
                }
                if (entry.order.side == Side::Sell && entry.order.price <= price) {
                    supply += entry.order.remaining;
                }
            }
            const Uncross here{.price = price,
                               .volume = std::min(demand, supply),
                               .imbalance = demand - supply};
            if (here.volume == 0) {
                continue;
            }
            const auto key = [reference](const Uncross& u) {
                return std::tuple{-u.volume, u.imbalance < 0 ? -u.imbalance : u.imbalance,
                                  u.price < reference ? reference - u.price : u.price - reference,
                                  u.price};
            };
            if (!best || key(here) < key(*best)) {
                best = here;
            }
        }
        return best;
    }

    // Lines up every bid at or above the price, highest and then oldest first, against every ask
    // at or below it, lowest and then oldest first, and trades them in pairs at the price.
    std::optional<Uncross> uncross(Price reference, std::vector<Fill>& fills,
                                   std::vector<RestingOrder>& selfTrades) {
        const std::optional<Uncross> result = indicative(reference);
        if (!result) {
            return std::nullopt;
        }
        const Price price = result->price;
        std::vector<Entry> bids;
        std::vector<Entry> asks;
        for (const Entry& entry : entries_) {
            if (entry.order.side == Side::Buy && entry.order.price >= price) {
                bids.push_back(entry);
            } else if (entry.order.side == Side::Sell && entry.order.price <= price) {
                asks.push_back(entry);
            }
        }
        std::ranges::sort(bids, [](const Entry& a, const Entry& b) { return ahead(a, b); });
        std::ranges::sort(asks, [](const Entry& a, const Entry& b) { return ahead(a, b); });
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < bids.size() && j < asks.size()) {
            Entry& bid = bids[i];
            Entry& ask = asks[j];
            const bool bidIsOlder = bid.sequence < ask.sequence;
            if (bid.order.owner == ask.order.owner) {
                Entry& newer = bidIsOlder ? ask : bid;
                selfTrades.push_back(newer.order);
                static_cast<void>(cancel(newer.order.id));
                (bidIsOlder ? j : i) += 1;
                continue;
            }
            Entry& maker = bidIsOlder ? bid : ask;
            Entry& taker = bidIsOlder ? ask : bid;
            const Quantity quantity = std::min(bid.order.remaining, ask.order.remaining);
            maker.order.remaining -= quantity;
            taker.order.remaining -= quantity;
            fills.push_back({.makerOrderId = maker.order.id,
                             .takerOrderId = taker.order.id,
                             .makerOwner = maker.order.owner,
                             .takerOwner = taker.order.owner,
                             .takerSide = taker.order.side,
                             .price = price,
                             .quantity = quantity,
                             .makerRemaining = maker.order.remaining});
            for (Entry* side : {&bid, &ask}) {
                const auto entry = findEntry(side->order.id);
                entry->order.remaining = side->order.remaining;
                if (side->order.remaining == 0) {
                    entries_.erase(entry);
                }
            }
            if (bid.order.remaining == 0) {
                ++i;
            }
            if (ask.order.remaining == 0) {
                ++j;
            }
        }
        return result;
    }

    std::optional<RestingOrder> cancel(OrderId id) {
        const auto entry = findEntry(id);
        if (entry == entries_.end()) {
            return std::nullopt;
        }
        const RestingOrder order = entry->order;
        entries_.erase(entry);
        return order;
    }

    [[nodiscard]] std::optional<RestingOrder> find(OrderId id) const {
        const auto entry = std::ranges::find(entries_, id, &Entry::id);
        if (entry == entries_.end()) {
            return std::nullopt;
        }
        return entry->order;
    }

    [[nodiscard]] std::vector<RestingOrder> orders() const {
        std::vector<RestingOrder> result;
        result.reserve(entries_.size());
        for (const Entry& entry : entries_) {
            result.push_back(entry.order);
        }
        return result;
    }

    // Price levels on one side, best price first.
    [[nodiscard]] std::vector<LevelSummary> depth(Side side) const {
        std::map<Price, LevelSummary> byPrice;
        for (const Entry& entry : entries_) {
            if (entry.order.side != side) {
                continue;
            }
            LevelSummary& level = byPrice[entry.order.price];
            level.price = entry.order.price;
            level.quantity += entry.order.remaining;
            ++level.orderCount;
        }
        std::vector<LevelSummary> levels;
        for (const auto& entry : byPrice) {
            levels.push_back(entry.second);
        }
        if (side == Side::Buy) {
            std::ranges::reverse(levels); // highest bid first
        }
        return levels;
    }

private:
    struct Entry {
        RestingOrder order{};
        std::uint64_t sequence = 0; // arrival order, for time priority

        [[nodiscard]] OrderId id() const { return order.id; }
    };

    std::vector<Entry>::iterator findEntry(OrderId id) {
        return std::ranges::find(entries_, id, &Entry::id);
    }

    // The resting order an incoming order on `takerSide` would trade with first: best price,
    // then earliest arrival.
    std::vector<Entry>::iterator bestOpposite(Side takerSide) {
        const Side makerSide = opposite(takerSide);
        auto best = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->order.side == makerSide && (best == entries_.end() || ahead(*it, *best))) {
                best = it;
            }
        }
        return best;
    }

    static bool ahead(const Entry& a, const Entry& b) {
        if (a.order.price != b.order.price) {
            return a.order.side == Side::Buy ? a.order.price > b.order.price
                                             : a.order.price < b.order.price;
        }
        return a.sequence < b.sequence;
    }

    static bool accepts(const OrderRequest& taker, Price makerPrice) {
        return taker.side == Side::Buy ? makerPrice <= taker.price : makerPrice >= taker.price;
    }

    std::vector<Entry> entries_;
    std::uint64_t nextSequence_ = 0;
};

} // namespace crowdbook::test

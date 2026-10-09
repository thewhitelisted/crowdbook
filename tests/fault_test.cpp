// Makes allocations fail on purpose, at point after point, and checks what a failure leaves
// behind: an id map that keeps every entry, an order book that passes its audit and goes on
// trading, and a simulation that stops cleanly instead of running on half way through an event.
// Run under the sanitizers too, so anything a failure left dangling shows up there.
//
// This file replaces the global operator new, so it is a test program of its own.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/detail/id_map.hpp"
#include "crowdbook/order_book.hpp"
#include "crowdbook/random.hpp"
#include "crowdbook/scenario_file.hpp"
#include "crowdbook/session.hpp"

namespace {

// How many more allocations succeed before one fails; negative for none failing.
std::int64_t allocationsLeft = -1;

// Memory from malloc, or nullptr when this allocation is the one to fail or there is none.
void* allocate(std::size_t size) noexcept {
    if (allocationsLeft >= 0 && allocationsLeft-- == 0) {
        return nullptr;
    }
    return std::malloc(size == 0 ? 1 : size);
}

void* allocateOrThrow(std::size_t size) {
    if (void* memory = allocate(size)) {
        return memory;
    }
    throw std::bad_alloc{};
}

} // namespace

// Every form that is not over-aligned, so that whatever one of them allocates, another of them
// frees: the sanitizers' runtime has its own, and memory must not pass between the two.
void* operator new(std::size_t size) { return allocateOrThrow(size); }
void* operator new[](std::size_t size) { return allocateOrThrow(size); }
void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return allocate(size);
}
void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return allocate(size);
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t /*size*/) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t /*size*/) noexcept { std::free(memory); }
void operator delete(void* memory, const std::nothrow_t& /*tag*/) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t& /*tag*/) noexcept { std::free(memory); }

namespace crowdbook {
namespace {

// Runs `step` with allocation number `n` within it failing, counting from 0. Returns whether it
// failed for want of memory.
template <typename Step>
bool failingAllocation(std::int64_t n, Step&& step) {
    allocationsLeft = n;
    bool failed = false;
    try {
        step();
    } catch (const std::bad_alloc&) {
        failed = true;
    }
    allocationsLeft = -1;
    return failed;
}

TEST(FaultTest, AnIdMapThatCannotGrowKeepsEveryEntry) {
    detail::IdMap<std::int64_t, std::int64_t> map;
    int failures = 0;
    for (std::int64_t key = 0; key < 2'000; ++key) {
        if (failingAllocation(0, [&] { map.tryEmplace(key, 7 * key); })) {
            ++failures;
            ASSERT_EQ(map.size(), static_cast<std::size_t>(key));
            ASSERT_FALSE(map.contains(key));
            map.tryEmplace(key, 7 * key);
        }
        if (failingAllocation(0, [&] { map.reserve(map.size() * 4); })) {
            ++failures;
        }
    }
    EXPECT_GT(failures, 10);
    ASSERT_EQ(map.size(), 2'000U);
    for (std::int64_t key = 0; key < 2'000; ++key) {
        ASSERT_EQ(map.at(key), 7 * key);
    }
}

// Random orders, modifies, cancels and call auctions against a new book, each with one of its
// first few allocations failing half the time. After every request the book must pass its audit,
// and at the end every order it holds must still cancel. When the fills have room, a request that
// fails must change nothing at all, since the book makes room before it trades; and a cancel never
// allocates, so it never fails. A book allocates most while it is new, so each is short-lived.
void tradeOnABookRunningOutOfMemory(Random& random, int requests, int& failures) {
    OrderBook book;
    std::vector<OrderId> ids;
    OrderId nextId = 1;
    const auto anyId = [&] {
        return ids[static_cast<std::size_t>(
            random.uniformInt(0, static_cast<std::int64_t>(ids.size()) - 1))];
    };
    for (int request = 0; request < requests; ++request) {
        std::vector<Fill> fills;
        // Half the time the fills have room, the other half recording one allocates too.
        const bool roomForFills = random.uniform() < 0.5;
        if (roomForFills) {
            fills.reserve(64);
        }
        const std::int64_t failAt = random.uniform() < 0.5 ? random.uniformInt(0, 3) : -1;
        const double kind = random.uniform();
        if (kind < 0.55 || ids.empty()) {
            const OrderRequest order{
                .id = nextId++,
                .owner = static_cast<AgentId>(random.uniformInt(1, 6)),
                .side = random.uniform() < 0.5 ? Side::Buy : Side::Sell,
                .type = random.uniform() < 0.1 ? OrderType::Market : OrderType::Limit,
                .timeInForce = random.uniform() < 0.1 ? TimeInForce::ImmediateOrCancel
                                                      : TimeInForce::GoodTillCancel,
                .price = 100 + random.uniformInt(-12, 12),
                .quantity = random.uniformInt(1, 12)};
            std::optional<OrderResult> result;
            if (failingAllocation(failAt, [&] { result = book.submit(order, fills); })) {
                ++failures;
                if (roomForFills) {
                    EXPECT_TRUE(fills.empty()) << "request " << request;
                    EXPECT_FALSE(book.find(order.id)) << "request " << request;
                }
            } else if (result->status == OrderStatus::Resting) {
                ids.push_back(order.id);
            }
        } else if (kind < 0.8) {
            const OrderId id = anyId();
            const std::optional<RestingOrder> before = book.find(id);
            const Price price = 100 + random.uniformInt(-12, 12);
            const Quantity quantity = random.uniformInt(1, 12);
            const auto modify = [&] {
                static_cast<void>(book.modify(id, price, quantity, fills));
            };
            if (failingAllocation(failAt, modify)) {
                ++failures;
                if (roomForFills) {
                    const std::optional<RestingOrder> after = book.find(id);
                    EXPECT_TRUE(fills.empty()) << "request " << request;
                    ASSERT_TRUE(after) << "request " << request;
                    EXPECT_EQ(after->price, before->price);
                    EXPECT_EQ(after->remaining, before->remaining);
                }
            }
        } else if (kind < 0.97) {
            const OrderId id = anyId();
            EXPECT_FALSE(failingAllocation(failAt, [&] { static_cast<void>(book.cancel(id)); }))
                << "a cancel allocated, at request " << request;
        } else if (book.matching()) {
            // A call auction: orders rest without trading until the book uncrosses.
            book.setMatching(false);
        } else {
            std::vector<RestingOrder> selfTrades;
            const auto uncross = [&] {
                while (book.uncross(100, fills, selfTrades) && !selfTrades.empty()) {
                    selfTrades.clear();
                }
            };
            if (failingAllocation(failAt, uncross)) {
                // The book is left part way through the auction, and finishes it.
                ++failures;
                uncross();
            }
            book.setMatching(true);
        }
        // Forget the ids that are no longer on the book, by asking the book itself.
        std::erase_if(ids, [&](OrderId id) { return !book.find(id); });
        const std::optional<std::string> problem = book.audit();
        ASSERT_FALSE(problem) << "after request " << request << ": " << *problem;
    }
    book.setMatching(true);
    for (const OrderId id : ids) {
        ASSERT_TRUE(book.cancel(id));
    }
    EXPECT_EQ(book.orderCount(), 0U);
    EXPECT_FALSE(book.bestBid());
    EXPECT_FALSE(book.bestAsk());
}

TEST(FaultTest, AnOrderBookThatRunsOutOfMemoryStaysConsistent) {
    Random random{17, 0};
    int failures = 0;
    for (int book = 0; book < 400 && !HasFatalFailure(); ++book) {
        tradeOnABookRunningOutOfMemory(random, 250, failures);
    }
    EXPECT_GT(failures, 2'000);
}

// An order that trades and then rests, at every size of book, so that resting it sometimes needs
// the index, the nodes or the levels to grow: whichever allocation fails, it fails before the order
// trades, and leaves the book as it was.
TEST(FaultTest, AnOrderThatTradesThenRestsHasItsRoomMadeFirst) {
    int failures = 0;
    for (OrderId resting = 1; resting <= 70; ++resting) {
        for (std::int64_t n = 0; n < 8; ++n) {
            OrderBook book;
            std::vector<Fill> fills;
            fills.reserve(8);
            for (OrderId id = 1; id < resting; ++id) {
                static_cast<void>(book.submit({.id = id,
                                               .owner = 1,
                                               .side = Side::Buy,
                                               .type = OrderType::Limit,
                                               .price = 50 - static_cast<Price>(id % 20),
                                               .quantity = 1},
                                              fills));
            }
            static_cast<void>(book.submit(
                {.id = resting, .owner = 1, .side = Side::Sell, .price = 100, .quantity = 1},
                fills));
            const OrderRequest taker{
                .id = 1'000, .owner = 2, .side = Side::Buy, .price = 100, .quantity = 5};
            if (failingAllocation(n, [&] { static_cast<void>(book.submit(taker, fills)); })) {
                ++failures;
                EXPECT_TRUE(fills.empty()) << resting << " resting, allocation " << n;
                EXPECT_TRUE(book.find(resting)) << resting << " resting, allocation " << n;
                EXPECT_FALSE(book.find(taker.id)) << resting << " resting, allocation " << n;
            }
            EXPECT_FALSE(book.audit());
        }
    }
    EXPECT_GT(failures, 20);
}

std::string example(const std::string& path) { return std::string{CROWDBOOK_SOURCE_DIR} + path; }

// A whole market, its allocation number n failing during a busy minute: the failure stops it,
// it refuses to go on, its book is consistent, and it is destroyed cleanly.
TEST(FaultTest, AMarketThatRunsOutOfMemoryStopsCleanly) {
    const Scenario scenario = loadScenario(example("/examples/challenges/market_making.toml"));
    int stopped = 0;
    for (std::int64_t n = 0; n < 4'000; n += 97) {
        SessionMarket market = openSession(scenario, AgentRegistry::withBuiltIns());
        market.run.runUntil(10 * kSecond);
        if (!failingAllocation(n, [&] { market.run.runUntil(70 * kSecond); })) {
            continue; // the minute needed fewer allocations than n
        }
        ++stopped;
        const Simulation& simulation = market.run.simulation();
        EXPECT_TRUE(simulation.failed());
        EXPECT_THROW(market.run.runUntil(80 * kSecond), std::logic_error);
        EXPECT_THROW(market.run.simulation().act(1, [](AgentContext&) {}), std::logic_error);
        const std::optional<std::string> problem = simulation.exchange().book().audit();
        EXPECT_FALSE(problem) << "failing allocation " << n << ": " << *problem;
    }
    EXPECT_GT(stopped, 10);
}

} // namespace
} // namespace crowdbook

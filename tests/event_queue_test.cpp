#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/detail/event_queue.hpp"
#include "crowdbook/random.hpp"

namespace crowdbook {
namespace {

using detail::EventQueue;

// What a reference queue holds: a time, the order it was scheduled in, and its payload.
using Item = std::tuple<Timestamp, std::uint64_t, int>;

TEST(EventQueueTest, TakesItemsOutEarliestFirstAndEqualTimesInOrder) {
    EventQueue<int> queue;
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.peekTime(), std::nullopt);
    for (const auto& [time, value] : {std::pair{30, 1}, {10, 2}, {30, 3}, {20, 4}, {10, 5}}) {
        queue.push(time) = value;
    }
    EXPECT_EQ(queue.size(), 5U);
    EXPECT_EQ(queue.peekTime(), 10);
    std::vector<std::pair<Timestamp, int>> out;
    while (!queue.empty()) {
        const auto taken = queue.pop();
        out.emplace_back(taken.time, queue.payload(taken.slot));
        queue.release(taken.slot);
    }
    EXPECT_EQ(out, (std::vector<std::pair<Timestamp, int>>{{10, 2}, {10, 5}, {20, 4}, {30, 1},
                                                            {30, 3}}));
}

TEST(EventQueueTest, APayloadStaysPutUntilReleased) {
    EventQueue<int> queue;
    queue.push(5) = 50;
    const auto taken = queue.pop();
    int& held = queue.payload(taken.slot);
    // Scheduling more while it is processed reuses no slot of an item not yet released.
    for (int i = 0; i < 1000; ++i) {
        queue.push(5 + i) = i;
    }
    EXPECT_EQ(&held, &queue.payload(taken.slot));
    EXPECT_EQ(held, 50);
    queue.release(taken.slot);
}

TEST(EventQueueTest, NothingCanBeScheduledBeforeTheItemLastTakenOut) {
    EventQueue<int> queue;
    queue.push(100);
    queue.release(queue.pop().slot);
    EXPECT_THROW(queue.push(99), std::logic_error);
    EXPECT_TRUE(queue.empty());
    queue.push(100) = 1; // the same time is still to come
    EXPECT_EQ(queue.nextTime(), 100);
}

TEST(EventQueueTest, LookingAheadLeavesRoomToScheduleSooner) {
    // A simulation runs until a time, looks at the next item and stops before it; then something
    // is scheduled before that item, which must come out first.
    EventQueue<int> queue;
    queue.push(1'000) = 1;
    queue.push(10) = 2;
    queue.release(queue.pop().slot);
    EXPECT_EQ(queue.nextTime(), 1'000);
    queue.push(20) = 3;
    EXPECT_EQ(queue.peekTime(), 20);
    EXPECT_EQ(queue.nextTime(), 20);
    const auto taken = queue.pop();
    EXPECT_EQ(taken.time, 20);
    EXPECT_EQ(queue.payload(taken.slot), 3);
}

// Against a binary heap, through many random schedules, looks and takes, with clustered times so
// that many tie, as a simulation's do.
TEST(EventQueueTest, MatchesAHeapThroughRandomUse) {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        Random random{seed, 0};
        EventQueue<int> queue;
        std::vector<Item> heap;
        const auto later = [](const Item& a, const Item& b) { return a > b; };
        std::uint64_t sequence = 0;
        Timestamp now = 0;
        for (int step = 0; step < 20'000; ++step) {
            const auto choice = random.below(10);
            if (choice < 5 || heap.empty()) {
                const Timestamp time =
                    now + (random.below(4) == 0 ? 0
                                                : random.uniformInt(0, random.below(2) == 0
                                                                           ? 1'000
                                                                           : 1'000'000'000));
                const int value = static_cast<int>(sequence);
                queue.push(time) = value;
                heap.emplace_back(time, sequence++, value);
                std::ranges::push_heap(heap, later);
            } else if (choice < 6) {
                ASSERT_EQ(queue.nextTime(), std::get<0>(heap.front()));
                ASSERT_EQ(queue.peekTime(), std::get<0>(heap.front()));
            } else {
                std::ranges::pop_heap(heap, later);
                const Item expected = heap.back();
                heap.pop_back();
                const auto taken = queue.pop();
                ASSERT_EQ(taken.time, std::get<0>(expected)) << "seed " << seed;
                ASSERT_EQ(queue.payload(taken.slot), std::get<2>(expected)) << "seed " << seed;
                queue.release(taken.slot);
                now = taken.time;
            }
            ASSERT_EQ(queue.size(), heap.size());
        }
    }
}

} // namespace
} // namespace crowdbook

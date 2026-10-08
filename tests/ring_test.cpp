#include <cstdint>
#include <deque>

#include <gtest/gtest.h>

#include "crowdbook/detail/ring.hpp"
#include "crowdbook/random.hpp"

namespace crowdbook {
namespace {

// Against the standard deque, through growth and wrapping around the end of the array.
TEST(RingTest, AgreesWithADequeThroughRandomChanges) {
    detail::Ring<std::uint64_t> ring;
    std::deque<std::uint64_t> expected;
    Random random{3, 0};
    for (std::uint64_t step = 0; step < 20'000; ++step) {
        // Mostly adding at first, then mostly removing, so the ring grows while it wraps.
        const bool add = expected.empty() || random.below(100) < (step < 10'000 ? 60U : 40U);
        if (add) {
            ring.pushBack(step);
            expected.push_back(step);
        } else {
            ring.popFront();
            expected.pop_front();
        }
        ASSERT_EQ(ring.size(), expected.size());
        if (!expected.empty()) {
            ASSERT_EQ(ring.front(), expected.front());
            ASSERT_EQ(ring.back(), expected.back());
            const std::size_t middle = expected.size() / 2;
            ASSERT_EQ(ring[middle], expected[middle]);
        }
    }
}

} // namespace
} // namespace crowdbook

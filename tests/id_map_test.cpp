#include <cstdint>
#include <map>
#include <stdexcept>
#include <unordered_map>

#include <gtest/gtest.h>

#include "crowdbook/detail/id_map.hpp"
#include "crowdbook/random.hpp"

namespace crowdbook {
namespace {

using detail::IdMap;

TEST(IdMapTest, FindsWhatWasPutAndForgetsWhatWasErased) {
    IdMap<std::uint64_t, int> map;
    EXPECT_EQ(map.find(7), nullptr);
    EXPECT_FALSE(map.erase(7));
    EXPECT_TRUE(map.tryEmplace(7, 70).second);
    EXPECT_FALSE(map.tryEmplace(7, 71).second); // the first value stays
    EXPECT_EQ(map.at(7), 70);
    map.insertOrAssign(7, 72);
    EXPECT_EQ(map.at(7), 72);
    EXPECT_TRUE(map.contains(7));
    EXPECT_TRUE(map.erase(7));
    EXPECT_FALSE(map.contains(7));
    EXPECT_TRUE(map.empty());
    EXPECT_THROW(static_cast<void>(map.at(7)), std::out_of_range);
    // Zero and the largest key are keys like any other.
    map.tryEmplace(0, 1);
    map.tryEmplace(~std::uint64_t{0}, 2);
    EXPECT_EQ(map.at(0), 1);
    EXPECT_EQ(map.at(~std::uint64_t{0}), 2);
}

// Against the standard map, through growth and long runs of neighbouring entries: inserting and
// erasing keys from a small range keeps the table crowded, so erasing has to move entries back
// into the gaps it leaves without losing any.
TEST(IdMapTest, AgreesWithAStandardMapThroughRandomChanges) {
    for (const std::uint64_t range : {20U, 200U, 5'000U}) {
        IdMap<std::uint64_t, std::uint64_t> map;
        std::unordered_map<std::uint64_t, std::uint64_t> expected;
        Random random{17, range};
        for (std::uint64_t step = 0; step < 100'000; ++step) {
            const std::uint64_t key = random.below(range);
            switch (random.below(3)) {
            case 0:
                EXPECT_EQ(map.tryEmplace(key, step).second,
                          expected.try_emplace(key, step).second);
                break;
            case 1:
                EXPECT_EQ(map.erase(key), expected.erase(key) == 1);
                break;
            default: {
                const std::uint64_t* found = map.find(key);
                const auto wanted = expected.find(key);
                ASSERT_EQ(found != nullptr, wanted != expected.end()) << key;
                if (found != nullptr) {
                    EXPECT_EQ(*found, wanted->second);
                }
            }
            }
            ASSERT_EQ(map.size(), expected.size());
        }
        std::map<std::uint64_t, std::uint64_t> walked;
        map.forEach([&](std::uint64_t key, std::uint64_t value) { walked.emplace(key, value); });
        EXPECT_EQ(walked, (std::map<std::uint64_t, std::uint64_t>{expected.begin(),
                                                                  expected.end()}));
    }
}

} // namespace
} // namespace crowdbook

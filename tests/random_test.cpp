#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

#include "crowdbook/random.hpp"

namespace crowdbook {
namespace {

constexpr int kSamples = 200'000;

// Expected values come from an independent Python implementation of SplitMix64 and xoshiro256**,
// itself checked against the algorithms' published reference outputs. Matching them on every CI
// platform shows the streams are portable.
TEST(RandomTest, MatchesReferenceOutputs) {
    constexpr std::array<std::uint64_t, 4> kSeed42Stream7 = {
        0xf06762b0c6dc9948, 0x43213a33d5400d3a, 0xcaee8597ab941914, 0x3ed7cd0afa8ba05c};
    constexpr std::array<std::uint64_t, 4> kSeed0Stream0 = {
        0x99ec5f36cb75f2b4, 0xbf6e1f784956452a, 0x1a5f849d4933e6e0, 0x6aa594f1262d2d2c};

    Random first{42, 7};
    for (const std::uint64_t expected : kSeed42Stream7) {
        EXPECT_EQ(first.next(), expected);
    }
    Random second{0, 0};
    for (const std::uint64_t expected : kSeed0Stream0) {
        EXPECT_EQ(second.next(), expected);
    }
}

TEST(RandomTest, StreamsAreReproducibleAndDistinct) {
    Random a{1, 1};
    Random b{1, 1};
    Random otherStream{1, 2};
    Random otherSeed{2, 1};
    for (int i = 0; i < 100; ++i) {
        const std::uint64_t value = a.next();
        EXPECT_EQ(value, b.next());
        EXPECT_NE(value, otherStream.next());
        EXPECT_NE(value, otherSeed.next());
    }
}

TEST(RandomTest, BelowIsUniformOverItsRange) {
    Random random{3, 0};
    std::array<int, 10> counts{};
    for (int i = 0; i < kSamples; ++i) {
        ++counts.at(static_cast<std::size_t>(random.below(10)));
    }
    for (const int count : counts) {
        EXPECT_NEAR(count, kSamples / 10, kSamples / 100); // within 10% of the expected count
    }
    EXPECT_THROW(static_cast<void>(random.below(0)), std::invalid_argument);
}

TEST(RandomTest, UniformIntCoversInclusiveRangesIncludingTheExtremes) {
    Random random{4, 0};
    std::array<bool, 7> seen{};
    for (int i = 0; i < 1'000; ++i) {
        const std::int64_t value = random.uniformInt(-3, 3);
        ASSERT_GE(value, -3);
        ASSERT_LE(value, 3);
        seen.at(static_cast<std::size_t>(value + 3)) = true;
    }
    for (const bool hit : seen) {
        EXPECT_TRUE(hit);
    }
    EXPECT_EQ(random.uniformInt(5, 5), 5);
    constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    static_cast<void>(random.uniformInt(kMin, kMax)); // the full range must not overflow
    EXPECT_THROW(static_cast<void>(random.uniformInt(1, 0)), std::invalid_argument);
}

TEST(RandomTest, UniformAndBernoulliHaveTheRightMeans) {
    Random random{5, 0};
    double sum = 0.0;
    int successes = 0;
    for (int i = 0; i < kSamples; ++i) {
        const double u = random.uniform();
        ASSERT_GE(u, 0.0);
        ASSERT_LT(u, 1.0);
        sum += u;
        successes += random.bernoulli(0.3) ? 1 : 0;
    }
    EXPECT_NEAR(sum / kSamples, 0.5, 0.005);
    EXPECT_NEAR(static_cast<double>(successes) / kSamples, 0.3, 0.005);
    EXPECT_FALSE(random.bernoulli(0.0));
    EXPECT_TRUE(random.bernoulli(1.0));
}

TEST(RandomTest, ExponentialHasMeanOneOverRate) {
    Random random{6, 0};
    double sum = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        const double value = random.exponential(4.0);
        ASSERT_GE(value, 0.0);
        sum += value;
    }
    EXPECT_NEAR(sum / kSamples, 0.25, 0.005);
    EXPECT_THROW(static_cast<void>(random.exponential(0.0)), std::invalid_argument);
}

TEST(RandomTest, NormalHasTheRequestedMeanAndSpread) {
    Random random{7, 0};
    double sum = 0.0;
    double sumOfSquares = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        const double value = random.normal(3.0, 2.0);
        sum += value;
        sumOfSquares += value * value;
    }
    const double mean = sum / kSamples;
    const double variance = sumOfSquares / kSamples - mean * mean;
    EXPECT_NEAR(mean, 3.0, 0.03);
    EXPECT_NEAR(std::sqrt(variance), 2.0, 0.03);
    EXPECT_EQ(random.normal(1.5, 0.0), 1.5);
    EXPECT_THROW(static_cast<void>(random.normal(0.0, -1.0)), std::invalid_argument);
}

} // namespace
} // namespace crowdbook

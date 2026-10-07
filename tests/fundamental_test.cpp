#include <cmath>
#include <cstdint>
#include <stdexcept>

#include <gtest/gtest.h>

#include "crowdbook/fundamental.hpp"

namespace crowdbook {
namespace {

constexpr int kPaths = 4'000;

TEST(FundamentalTest, StartsAtItsInitialValueAndChangesOnlyAtSteps) {
    Fundamental fundamental{{.initial = 500.0, .volatility = 3.0, .step = kSecond}, Random{1, 0}};
    EXPECT_EQ(fundamental.valueAt(0), 500.0);
    EXPECT_EQ(fundamental.valueAt(kSecond - 1), 500.0);
    const double afterOneStep = fundamental.valueAt(kSecond);
    EXPECT_NE(afterOneStep, 500.0);
    EXPECT_EQ(fundamental.valueAt(2 * kSecond - 1), afterOneStep);
}

TEST(FundamentalTest, PathDoesNotDependOnHowOftenItIsRead) {
    const FundamentalConfig config{.volatility = 2.0, .step = 10 * kMillisecond};
    Fundamental readOften{config, Random{9, 0}};
    Fundamental readOnce{config, Random{9, 0}};
    for (Timestamp time = 0; time < 10 * kSecond; time += kMillisecond) {
        static_cast<void>(readOften.valueAt(time));
    }
    EXPECT_EQ(readOften.valueAt(10 * kSecond), readOnce.valueAt(10 * kSecond));
}

TEST(FundamentalTest, RandomWalkVarianceGrowsWithTime) {
    // Without mean reversion the change over T seconds has variance volatility^2 * T.
    double sum = 0.0;
    double sumOfSquares = 0.0;
    for (std::uint64_t path = 0; path < kPaths; ++path) {
        Fundamental fundamental{{.initial = 100.0, .volatility = 2.0, .step = 100 * kMillisecond},
                                Random{path, 0}};
        const double change = fundamental.valueAt(10 * kSecond) - 100.0;
        sum += change;
        sumOfSquares += change * change;
    }
    const double mean = sum / kPaths;
    EXPECT_NEAR(mean, 0.0, 0.5);
    EXPECT_NEAR(sumOfSquares / kPaths - mean * mean, 4.0 * 10.0, 4.0);
}

TEST(FundamentalTest, MeanReversionHoldsTheValueNearItsMean) {
    // An Ornstein-Uhlenbeck process settles at variance volatility^2 / (2 * meanReversion).
    double sum = 0.0;
    double sumOfSquares = 0.0;
    for (std::uint64_t path = 0; path < kPaths; ++path) {
        Fundamental fundamental{{.initial = 100.0,
                                 .meanReversion = 1.0,
                                 .volatility = 2.0,
                                 .step = 100 * kMillisecond},
                                Random{path, 0}};
        const double gap = fundamental.valueAt(20 * kSecond) - 100.0;
        sum += gap;
        sumOfSquares += gap * gap;
    }
    const double mean = sum / kPaths;
    EXPECT_NEAR(mean, 0.0, 0.15);
    EXPECT_NEAR(sumOfSquares / kPaths - mean * mean, 2.0, 0.2);
}

TEST(FundamentalTest, RejectsReadsBackInTimeAndInvalidConfigs) {
    Fundamental fundamental{{.step = kSecond}, Random{1, 0}};
    static_cast<void>(fundamental.valueAt(5 * kSecond));
    EXPECT_NO_THROW(static_cast<void>(fundamental.valueAt(5 * kSecond + 1))); // same step
    EXPECT_THROW(static_cast<void>(fundamental.valueAt(4 * kSecond)), std::invalid_argument);

    EXPECT_THROW((Fundamental{{.step = 0}, Random{1, 0}}), std::invalid_argument);
    EXPECT_THROW((Fundamental{{.volatility = -1.0}, Random{1, 0}}), std::invalid_argument);
    EXPECT_THROW((Fundamental{{.meanReversion = -1.0}, Random{1, 0}}), std::invalid_argument);
}

} // namespace
} // namespace crowdbook

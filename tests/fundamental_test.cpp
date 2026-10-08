#include <algorithm>
#include <array>
#include <utility>
#include <cmath>
#include <cstdint>
#include <limits>
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

TEST(FundamentalTest, NewsArrivesAsAPoissonProcessOfNormalJumps) {
    // No diffusion, so every change is a jump: one a second on average, 5 ticks each.
    Fundamental fundamental{{.initial = 1'000.0,
                             .volatility = 0.0,
                             .step = 100 * kMillisecond,
                             .jumpRate = 1.0,
                             .jumpSize = 5.0},
                            Random{4, 0}};
    double previous = fundamental.valueAt(0);
    double sumOfSquares = 0.0;
    for (Timestamp time = 100 * kMillisecond; time <= 10'000 * kSecond;
         time += 100 * kMillisecond) {
        const double value = fundamental.valueAt(time);
        sumOfSquares += (value - previous) * (value - previous);
        previous = value;
    }
    // 10,000 jumps expected, give or take 100; their sizes average 25 square ticks.
    EXPECT_NEAR(static_cast<double>(fundamental.jumps()), 10'000.0, 400.0);
    EXPECT_NEAR(sumOfSquares / static_cast<double>(fundamental.jumps()), 25.0, 1.5);
}

TEST(FundamentalTest, WithoutNewsThePathIsTheSameAsBefore) {
    // Turning news off makes no draws for it, so earlier results stay reproducible.
    Fundamental quiet{{.volatility = 2.0, .step = 10 * kMillisecond}, Random{9, 0}};
    Fundamental withRateOnly{{.volatility = 2.0, .step = 10 * kMillisecond, .jumpSize = 4.0},
                             Random{9, 0}};
    EXPECT_EQ(quiet.valueAt(100 * kSecond), withRateOnly.valueAt(100 * kSecond));
    EXPECT_EQ(quiet.jumps(), 0);
}

TEST(FundamentalTest, RejectsReadsBackInTimeAndInvalidConfigs) {
    Fundamental fundamental{{.step = kSecond}, Random{1, 0}};
    static_cast<void>(fundamental.valueAt(5 * kSecond));
    EXPECT_NO_THROW(static_cast<void>(fundamental.valueAt(5 * kSecond + 1))); // same step
    EXPECT_THROW(static_cast<void>(fundamental.valueAt(4 * kSecond)), std::invalid_argument);

    EXPECT_THROW((Fundamental{{.step = 0}, Random{1, 0}}), std::invalid_argument);
    EXPECT_THROW((Fundamental{{.volatility = -1.0}, Random{1, 0}}), std::invalid_argument);
    EXPECT_THROW((Fundamental{{.meanReversion = -1.0}, Random{1, 0}}), std::invalid_argument);
    EXPECT_THROW((Fundamental{{.jumpRate = -1.0}, Random{1, 0}}), std::invalid_argument);
    EXPECT_THROW((Fundamental{{.jumpSize = -1.0}, Random{1, 0}}), std::invalid_argument);
    constexpr double kInfinity = std::numeric_limits<double>::infinity();
    constexpr double kNotANumber = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((Fundamental{{.initial = kNotANumber}, Random{1, 0}}), std::invalid_argument);
    EXPECT_THROW((Fundamental{{.initial = kInfinity}, Random{1, 0}}), std::invalid_argument);
    EXPECT_THROW((Fundamental{{.volatility = kInfinity}, Random{1, 0}}), std::invalid_argument);
    EXPECT_THROW((Fundamental{{.meanReversion = kInfinity}, Random{1, 0}}),
                 std::invalid_argument);
    EXPECT_THROW((Fundamental{{.jumpRate = kInfinity}, Random{1, 0}}), std::invalid_argument);
    EXPECT_THROW((Fundamental{{.jumpSize = kInfinity}, Random{1, 0}}), std::invalid_argument);
}

// A prediction market's value starts at its probability, stays a probability, and at resolution
// is 100 or 0 for good.
TEST(PredictionTest, StartsAtItsProbabilityAndResolvesToZeroOrOneHundred) {
    for (const PredictionConfig& config :
         {PredictionConfig{.probability = 0.5},
          PredictionConfig{.probability = 0.2, .newsRate = 0.05, .newsShare = 0.6},
          PredictionConfig{.probability = 0.93, .newsRate = 0.2, .newsShare = 0.3,
                           .step = 250 * kMillisecond}}) {
        Fundamental value{config, 100 * kSecond, Random{3, 0}};
        EXPECT_EQ(value.resolution(), 100 * kSecond);
        EXPECT_NEAR(value.valueAt(0), 100.0 * config.probability, 1e-9);
        for (Timestamp time = 0; time < 100 * kSecond; time += 700 * kMillisecond) {
            const double probability = value.valueAt(time);
            EXPECT_GE(probability, 0.0);
            EXPECT_LE(probability, 100.0);
        }
        const double outcome = value.valueAt(100 * kSecond);
        EXPECT_TRUE(outcome == 0.0 || outcome == 100.0);
        EXPECT_EQ(value.valueAt(200 * kSecond), outcome);
    }
    EXPECT_FALSE(Fundamental(FundamentalConfig{}, Random{3, 0}).resolution());
}

// The value is a fair price: whatever it says at any moment is how often the market resolves yes
// from there. Checked over thousands of runs at the start and halfway, binned by the value then,
// with news and without, and for a run that does not end on a whole step.
TEST(PredictionTest, TheValueIsTheProbabilityOfYes) {
    constexpr int kRuns = 4'000;
    struct Case {
        PredictionConfig config;
        Duration resolution;
        Timestamp readAt;
    };
    for (const auto& [config, resolution, readAt] :
         {Case{PredictionConfig{.probability = 0.3, .step = kSecond}, 60 * kSecond, 30 * kSecond},
          Case{PredictionConfig{.probability = 0.6, .newsRate = 0.1, .newsShare = 0.7,
                                .step = kSecond},
               60'500 * kMillisecond, 30 * kSecond},
          // The last step is a third of the run, and only partly happens before resolution.
          Case{PredictionConfig{.probability = 0.5, .step = 10 * kSecond}, 15 * kSecond,
               10 * kSecond}}) {
        std::array<double, 5> valued{};
        std::array<double, 5> resolved{};
        std::array<int, 5> count{};
        double yes = 0.0;
        for (int run = 0; run < kRuns; ++run) {
            Fundamental value{config, resolution, Random{static_cast<std::uint64_t>(run), 0}};
            const double halfway = value.valueAt(readAt);
            const double outcome = value.valueAt(resolution);
            const auto bin = std::min<std::size_t>(static_cast<std::size_t>(halfway / 20.0), 4);
            valued[bin] += halfway;
            resolved[bin] += outcome;
            ++count[bin];
            yes += outcome;
        }
        // A share of outcomes worth 0 or 100 has standard error 100 √(p (1 - p) / n) at most.
        EXPECT_NEAR(yes / kRuns, 100.0 * config.probability, 4.0 * 50.0 / std::sqrt(kRuns));
        for (std::size_t bin = 0; bin < 5; ++bin) {
            if (count[bin] < 200) {
                continue;
            }
            EXPECT_NEAR(resolved[bin] / count[bin], valued[bin] / count[bin],
                        4.0 * 50.0 / std::sqrt(count[bin]))
                << "values from " << 20 * bin << " to " << 20 * (bin + 1);
        }
    }
}

// The hidden quantity's change over the run has variance 1, and news carries its share of it.
TEST(PredictionTest, TheHiddenQuantityMovesAsConfigured) {
    constexpr int kRuns = 3'000;
    const PredictionConfig config{.probability = 0.5, .newsRate = 0.1, .newsShare = 0.7};
    double total = 0.0;
    double fromNewsSteps = 0.0;
    for (int run = 0; run < kRuns; ++run) {
        Fundamental value{config, 60 * kSecond, Random{static_cast<std::uint64_t>(run), 7}};
        const double start = value.walk();
        double previous = start;
        std::int64_t jumps = 0;
        for (Timestamp time = kSecond; time <= 60 * kSecond; time += kSecond) {
            static_cast<void>(value.valueAt(time));
            const double move = value.walk() - previous;
            if (value.jumps() > jumps) {
                fromNewsSteps += move * move;
            }
            jumps = value.jumps();
            previous = value.walk();
        }
        total += (value.walk() - start) * (value.walk() - start);
    }
    EXPECT_NEAR(total / kRuns, 1.0, 0.1);
    // Steps with news carry the news's 70%, and the steady wandering of those tenth of the steps.
    EXPECT_NEAR(fromNewsSteps / total, 0.7 + 0.1 * 0.3, 0.05);
}

// As resolution comes near, the same news moves the probability further: the value moves
// slowly while much is unknown and fast at the end.
TEST(PredictionTest, TheValueMovesFasterAsResolutionNears) {
    double early = 0.0;
    double late = 0.0;
    for (std::uint64_t run = 0; run < 400; ++run) {
        Fundamental value{PredictionConfig{.probability = 0.5}, 100 * kSecond, Random{run, 0}};
        double previous = value.valueAt(0);
        for (Timestamp time = kSecond; time < 100 * kSecond; time += kSecond) {
            const double now = value.valueAt(time);
            const double move = (now - previous) * (now - previous);
            (time <= 10 * kSecond ? early : late) += time <= 10 * kSecond || time > 90 * kSecond
                                                         ? move
                                                         : 0.0;
            previous = now;
        }
    }
    EXPECT_GT(late, 3.0 * early);
}

TEST(PredictionTest, SettingsAreChecked) {
    for (const PredictionConfig& config :
         {PredictionConfig{.probability = 0.0}, PredictionConfig{.probability = 1.0},
          PredictionConfig{.newsRate = -1.0}, PredictionConfig{.newsShare = 0.5},
          PredictionConfig{.newsRate = 1.0, .newsShare = 1.0},
          PredictionConfig{.step = 0}, PredictionConfig{.newsRate = 10.0, .newsShare = 0.5}}) {
        EXPECT_THROW(Fundamental(config, 100 * kSecond, Random{1, 0}), std::invalid_argument);
    }
    EXPECT_THROW(Fundamental(PredictionConfig{}, 0, Random{1, 0}), std::invalid_argument);
    Fundamental value{PredictionConfig{}, 100 * kSecond, Random{1, 0}};
    static_cast<void>(value.valueAt(50 * kSecond));
    EXPECT_THROW(static_cast<void>(value.valueAt(10 * kSecond)), std::invalid_argument);
}

} // namespace
} // namespace crowdbook

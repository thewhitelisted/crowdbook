#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/agents/belief.hpp"
#include "crowdbook/fundamental.hpp"
#include "fake_context.hpp"

namespace crowdbook {
namespace {

using test::FakeContext;

// A value that moves a lot every 100 ms, so that each step's value is different.
const FundamentalConfig kWalk{.initial = 1'000.0, .volatility = 10.0, .step = 100 * kMillisecond};

std::shared_ptr<Fundamental> walk() { return std::make_shared<Fundamental>(kWalk, Random{4, 0}); }

// The value can be read back in time as far as it was asked to remember, and gives what it gave.
TEST(FundamentalMemoryTest, ReadsBackAsFarAsItRemembers) {
    Fundamental value{kWalk, Random{4, 0}};
    // Two readers ask for different windows: the longer one is kept. A window that ends inside a
    // step reaches back into that step: 2.05 s before 5.04 s, in step 50, is 2.99 s, in step 29.
    value.remember(2 * kSecond + 50 * kMillisecond);
    value.remember(kSecond);
    std::vector<double> seen;
    for (Timestamp time = 0; time <= 5 * kSecond; time += 100 * kMillisecond) {
        seen.push_back(value.valueAt(time));
    }
    for (int step = 29; step <= 50; ++step) {
        EXPECT_EQ(value.valueAt(step * 100 * kMillisecond), seen[static_cast<std::size_t>(step)]);
    }
    EXPECT_EQ(value.valueAt(5 * kSecond + 40 * kMillisecond - 2 * kSecond - 50 * kMillisecond),
              seen[29]);
    EXPECT_THROW(static_cast<void>(value.valueAt(2 * kSecond + 99 * kMillisecond)),
                 std::invalid_argument);

    // A prediction market's probability, read late, is what it was then.
    Fundamental question{PredictionConfig{.probability = 0.4, .newsRate = 0.1, .newsShare = 0.5},
                         60 * kSecond, Random{4, 0}};
    question.remember(10 * kSecond);
    std::vector<double> probabilities;
    for (Timestamp time = 0; time <= 20 * kSecond; time += kSecond) {
        probabilities.push_back(question.valueAt(time));
    }
    for (int second = 10; second <= 20; ++second) {
        EXPECT_EQ(question.valueAt(second * kSecond),
                  probabilities[static_cast<std::size_t>(second)]);
    }
    // Steps nobody read when they were current are worked out when first read late.
    Fundamental skipping{PredictionConfig{.probability = 0.4, .newsRate = 0.1, .newsShare = 0.5},
                         60 * kSecond, Random{4, 0}};
    skipping.remember(10 * kSecond);
    for (Timestamp time = 0; time <= 20 * kSecond; time += 2 * kSecond) {
        static_cast<void>(skipping.valueAt(time));
    }
    for (int second = 11; second <= 19; second += 2) {
        EXPECT_EQ(skipping.valueAt(second * kSecond),
                  probabilities[static_cast<std::size_t>(second)]);
    }

    // After the question resolves, late readers still see the probability as it was: the walk
    // that decided it went on past the last step they can read.
    Fundamental resolving{PredictionConfig{.probability = 0.5}, 10 * kSecond + 500 * kMillisecond,
                          Random{4, 0}};
    resolving.remember(5 * kSecond);
    const double lastStep = resolving.valueAt(10 * kSecond);
    static_cast<void>(resolving.valueAt(9 * kSecond));
    EXPECT_EQ(resolving.valueAt(11 * kSecond), resolving.walk() > 0.0 ? 100.0 : 0.0);
    EXPECT_EQ(resolving.valueAt(10 * kSecond + 200 * kMillisecond), lastStep);
}

// The step a look saw: the one whose value is the estimate, searched back from now.
std::optional<std::int64_t> stepsLate(Fundamental& truth, double estimate, Timestamp now) {
    const std::int64_t current = now / kWalk.step;
    for (std::int64_t back = 0; back <= current; ++back) {
        if (truth.valueAt((current - back) * kWalk.step) == estimate) {
            return back;
        }
    }
    return std::nullopt;
}

// Each trader sees the value late by a delay of its own, which stays the same; across a group the
// delays run from none to twice the setting.
TEST(BeliefTest, EachTraderSeesTheValueLateByItsOwnDelay) {
    const BeliefConfig config{.noise = 0.0, .lag = 2 * kSecond};
    double total = 0.0;
    constexpr int kTraders = 200;
    for (int trader = 0; trader < kTraders; ++trader) {
        const auto value = walk();
        Belief belief{config, value};
        // A copy of the same path, to look the estimates up in.
        auto truth = walk();
        truth->remember(10 * kSecond);
        FakeContext context{static_cast<std::uint64_t>(trader) + 1};
        std::optional<std::int64_t> delay;
        for (Timestamp now = 5 * kSecond; now <= 9 * kSecond; now += 700 * kMillisecond) {
            context.setNow(now);
            const double estimate = belief.look(context);
            const auto late = stepsLate(*truth, estimate, now);
            ASSERT_TRUE(late);
            if (delay) {
                // Its delay in steps can round either way as `now` moves within a step.
                EXPECT_LE(std::abs(*late - *delay), 1);
            }
            delay = late;
            EXPECT_LE(*late, 2 * config.lag / kWalk.step + 1);
        }
        total += static_cast<double>(*delay * kWalk.step);
    }
    // The mean delay is the setting, give or take a step and the spread of 200 draws.
    EXPECT_NEAR(total / kTraders, static_cast<double>(config.lag),
                static_cast<double>(kWalk.step) + 0.15 * static_cast<double>(config.lag));
}

// A lasting error has the size asked for and fades over its memory; the bias moves every estimate
// by exactly itself.
TEST(BeliefTest, ErrorsLastAndBiasTilts) {
    const auto value = walk();
    {
        Belief tilted{BeliefConfig{.noise = 0.0, .bias = -7.5}, value};
        FakeContext context;
        context.setNow(3 * kSecond);
        EXPECT_EQ(tilted.look(context), value->valueAt(3 * kSecond) - 7.5);
    }
    const BeliefConfig config{.noise = 0.0, .error = 4.0, .errorMemory = 10 * kSecond};
    constexpr int kTraders = 2'000;
    double sumSquares = 0.0;
    double laterSquares = 0.0;
    double product = 0.0;
    for (int trader = 0; trader < kTraders; ++trader) {
        const auto own = walk();
        Belief belief{config, own};
        FakeContext context{static_cast<std::uint64_t>(trader) + 1};
        context.setNow(10 * kSecond);
        const double first = belief.look(context) - own->valueAt(10 * kSecond);
        context.setNow(20 * kSecond);
        const double second = belief.look(context) - own->valueAt(20 * kSecond);
        sumSquares += first * first;
        laterSquares += second * second;
        product += first * second;
    }
    // Its size stays the same as it wanders.
    EXPECT_NEAR(std::sqrt(sumSquares / kTraders), 4.0, 0.2);
    EXPECT_NEAR(std::sqrt(laterSquares / kTraders), 4.0, 0.2);
    // Ten seconds apart, one memory: a correlation of 1/e.
    EXPECT_NEAR(product / sumSquares, std::exp(-1.0), 0.05);
}

TEST(BeliefTest, SettingsAreChecked) {
    const auto value = walk();
    for (const BeliefConfig& config :
         {BeliefConfig{.noise = -1.0}, BeliefConfig{.error = -1.0}, BeliefConfig{.lag = -1},
          BeliefConfig{.errorMemory = 0}, BeliefConfig{.bias = std::nan("")},
          BeliefConfig{.lag = kMaxDuration + 1}}) {
        EXPECT_THROW(Belief(config, value), std::invalid_argument);
    }
    EXPECT_THROW(Belief(BeliefConfig{}, nullptr), std::invalid_argument);
}

} // namespace
} // namespace crowdbook

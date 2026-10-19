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

// A recorded path: the opening probability until the first point, each point's value from its
// time until the next, and the outcome at resolution, the last point's without one. It is a
// function of time alone, so a late read sees what was recorded then.
TEST(PredictionTest, ARecordedPathIsFollowedAndSettles) {
    const PredictionConfig config{.probability = 0.4,
                                  .path = {{.at = 10 * kSecond, .value = 45.0},
                                           {.at = 30 * kSecond, .value = 61.5},
                                           {.at = 30 * kSecond, .value = 63.0},
                                           {.at = 80 * kSecond, .value = 90.0}},
                                  .outcome = 100.0};
    Fundamental value{config, 100 * kSecond, Random{1, 0}};
    EXPECT_NEAR(value.valueAt(0), 40.0, 1e-12);
    EXPECT_NEAR(value.valueAt(10 * kSecond - 1), 40.0, 1e-12);
    EXPECT_NEAR(value.valueAt(10 * kSecond), 45.0, 1e-12);
    EXPECT_NEAR(value.valueAt(30 * kSecond), 63.0, 1e-12); // the later of two at one time
    EXPECT_NEAR(value.valueAt(85 * kSecond), 90.0, 1e-12);
    EXPECT_NEAR(value.valueAt(20 * kSecond), 45.0, 1e-12); // read late
    EXPECT_NEAR(value.valueAt(100 * kSecond), 100.0, 1e-12);
    EXPECT_NEAR(value.valueAt(200 * kSecond), 100.0, 1e-12);
    EXPECT_EQ(value.jumps(), 0);

    Fundamental unsettled{PredictionConfig{.probability = 0.4, .path = {{5 * kSecond, 70.0}}},
                          100 * kSecond, Random{1, 0}};
    EXPECT_NEAR(unsettled.valueAt(100 * kSecond), 70.0, 1e-12);
    Fundamental pathless{PredictionConfig{.probability = 0.4, .outcome = 0.0}, 100 * kSecond,
                         Random{1, 0}};
    EXPECT_NEAR(pathless.valueAt(99 * kSecond), 40.0, 1e-12);
    EXPECT_NEAR(pathless.valueAt(100 * kSecond), 0.0, 1e-12);
}

// Outside a prediction market a recorded path is in ticks, `initial` until its first point,
// and holds its last value: there is nothing to settle.
TEST(FundamentalTest, ARecordedPathIsFollowedInTicks) {
    Fundamental value{FundamentalConfig{.initial = 6000.0,
                                        .volatility = 0.0,
                                        .path = {{.at = 10 * kSecond, .value = 6012.5},
                                                 {.at = 30 * kSecond, .value = 5990.0}}},
                      Random{1, 0}};
    EXPECT_NEAR(value.valueAt(0), 6000.0, 1e-12);
    EXPECT_NEAR(value.valueAt(10 * kSecond), 6012.5, 1e-12);
    EXPECT_NEAR(value.valueAt(31 * kSecond), 5990.0, 1e-12);
    EXPECT_NEAR(value.valueAt(15 * kSecond), 6012.5, 1e-12); // read late
    EXPECT_NEAR(value.valueAt(1000 * kSecond), 5990.0, 1e-12);
    for (const FundamentalConfig& config :
         {FundamentalConfig{.volatility = 1.0, .path = {{.at = 0, .value = 1.0}}},
          FundamentalConfig{.volatility = 0.0, .jumpRate = 0.1, .path = {{.at = 0, .value = 1.0}}},
          FundamentalConfig{.volatility = 0.0,
                            .path = {{.at = 2 * kSecond, .value = 1.0}, {.at = kSecond, .value = 1.0}}},
          FundamentalConfig{.volatility = 0.0, .path = {{.at = -1, .value = 1.0}}}}) {
        EXPECT_THROW(Fundamental(config, Random{1, 0}), std::invalid_argument);
    }
}

TEST(PredictionTest, ARecordedPathIsChecked) {
    const std::vector<ValuePoint> path{{.at = kSecond, .value = 50.0}};
    for (const PredictionConfig& config :
         {PredictionConfig{.path = {{.at = 2 * kSecond, .value = 50.0}, {.at = kSecond}}},
          PredictionConfig{.path = {{.at = -1, .value = 50.0}}},
          PredictionConfig{.path = {{.at = kSecond, .value = 101.0}}},
          PredictionConfig{.path = path, .outcome = -1.0},
          PredictionConfig{.newsRate = 0.1, .path = path},
          PredictionConfig{.game = GameConfig{}, .path = path}}) {
        EXPECT_THROW(Fundamental(config, 100 * kSecond, Random{1, 0}), std::invalid_argument);
    }
}

GameConfig smallGame() { return {.start = 10 * kSecond, .length = 60 * kSecond, .scores = 3.0}; }

// A game's market opens at the home side's chance asked for, whatever a score is worth and
// whatever a tie pays.
TEST(GameTest, StartsAtTheProbabilityAskedFor) {
    for (const auto& [probability, game] :
         {std::pair{0.5, smallGame()}, std::pair{0.2, smallGame()},
          std::pair{0.7, GameConfig{.length = 60 * kSecond,
                                    .scores = 90.0,
                                    .points = {1, 2, 3},
                                    .pointWeights = {0.2, 0.6, 0.2}}},
          std::pair{0.5, GameConfig{.length = 60 * kSecond, .scores = 2.0, .tie = 50.0}}}) {
        Fundamental value{PredictionConfig{.probability = probability, .game = game}, 100 * kSecond,
                          Random{1, 0}};
        EXPECT_NEAR(value.valueAt(0), 100.0 * probability, 1e-6);
        EXPECT_EQ(value.walk(), 0.0);
    }
}

// A contract can pay on the away side's win or on a draw: each opens at its price, and settles on
// its own result.
TEST(GameTest, ContractsOnAnAwayWinOrADrawOpenAtTheirPriceAndSettleOnTheirResult) {
    for (const auto& [side, probability] :
         {std::pair{GameSide::Away, 0.35}, std::pair{GameSide::Draw, 0.2}}) {
        GameConfig game = smallGame();
        game.side = side;
        int settled = 0;
        for (std::uint64_t path = 0; path < 40; ++path) {
            Fundamental value{PredictionConfig{.probability = probability, .game = game},
                              80 * kSecond, Random{path, 9}};
            EXPECT_NEAR(value.valueAt(0), 100.0 * probability, 1e-6);
            const double result = value.valueAt(80 * kSecond);
            const bool paid = side == GameSide::Away ? value.walk() < 0 : value.walk() == 0;
            EXPECT_EQ(result, paid ? 100.0 : 0.0);
            settled += paid ? 1 : 0;
        }
        EXPECT_GT(settled, 0);
    }
    // A draw's price cannot be above what an even game gives, about 24% with 3 scores expected.
    GameConfig draw = smallGame();
    draw.side = GameSide::Draw;
    EXPECT_THROW(
        Fundamental(PredictionConfig{.probability = 0.6, .game = draw}, 80 * kSecond, Random{1, 0}),
        std::invalid_argument);
}

// A total over or under a line opens at its price, the expected scoring set to give it, and
// settles on the points scored; a spread opens and settles on the home side's lead against it.
TEST(GameTest, TotalsAndSpreadsOpenAtTheirPriceAndSettleOnTheirLine) {
    struct Case {
        GameSide side;
        double line;
        double probability;
    };
    for (const auto& [side, line, probability] :
         {Case{GameSide::Over, 2.5, 0.45}, Case{GameSide::Under, 2.5, 0.6},
          Case{GameSide::HomeSpread, 1.5, 0.3}, Case{GameSide::AwaySpread, -0.5, 0.4}}) {
        GameConfig game = smallGame();
        game.side = side;
        game.line = line;
        int paid = 0;
        for (std::uint64_t path = 0; path < 300; ++path) {
            Fundamental value{PredictionConfig{.probability = probability, .game = game},
                              80 * kSecond, Random{path, 11}};
            EXPECT_NEAR(value.valueAt(0), 100.0 * probability, 1e-6);
            const double result = value.valueAt(80 * kSecond);
            EXPECT_TRUE(result == 0.0 || result == 100.0);
            paid += result == 100.0 ? 1 : 0;
        }
        // Without drift the price is the chance it pays, give or take the spread of 300 games.
        EXPECT_NEAR(paid / 300.0, probability, 4 * std::sqrt(probability * (1 - probability) / 300))
            << static_cast<int>(side);
    }
}

// Before kickoff a total's price moves with the drift and the news, which scale how much is
// expected to be scored, and it stays fair: over many games, what the total pays matches its
// price at the opening and at kickoff. A win, whose strength drifts the same way, too.
TEST(GameTest, ATotalDriftsBeforeKickoffAndStaysFair) {
    for (const GameSide side : {GameSide::Over, GameSide::Home}) {
        const GameConfig game{.side = side,
                              .line = side == GameSide::Over ? 2.5 : 0.0,
                              .start = 60 * kSecond,
                              .length = 60 * kSecond,
                              .scores = 2.5,
                              .pregameDrift = 0.5,
                              .news = 2.0,
                              .newsSize = 0.3};
        double paid = 0.0;
        double atKickoff = 0.0;
        bool moved = false;
        constexpr int kGames = 4000;
        for (int run = 0; run < kGames; ++run) {
            Fundamental value{PredictionConfig{.probability = 0.45, .game = game}, 130 * kSecond,
                              Random{11, static_cast<std::uint64_t>(run)}};
            EXPECT_NEAR(value.valueAt(0), 45.0, 1e-6);
            const double kickoff = value.valueAt(60 * kSecond);
            moved |= std::abs(kickoff - 45.0) > 1.0;
            atKickoff += kickoff;
            paid += value.valueAt(130 * kSecond);
        }
        EXPECT_TRUE(moved);
        // The price at kickoff averages the opening one, and the payout the price: within about
        // three standard errors of 4000 games.
        EXPECT_NEAR(atKickoff / kGames, 45.0, 1.5);
        EXPECT_NEAR(paid / kGames, 45.0, 2.5);
    }
}

// A total so high that the chance of no score underflows is priced all the same, its points to
// come taken as normal, and settles on its line.
TEST(GameTest, AVeryHighTotalIsPriced) {
    const GameConfig game{.side = GameSide::Over,
                          .line = 1000.5,
                          .length = 60 * kSecond,
                          .scores = 10.0,
                          .points = {1, 2, 3},
                          .pointWeights = {0.2, 0.6, 0.2}};
    Fundamental value{PredictionConfig{.probability = 0.5, .game = game}, 80 * kSecond,
                      Random{5, 0}};
    EXPECT_NEAR(value.valueAt(0), 50.0, 1e-6);
    for (Timestamp time = kSecond; time <= 60 * kSecond; time += 5 * kSecond) {
        const double price = value.valueAt(time);
        EXPECT_TRUE(price >= 0.0 && price <= 100.0) << price;
    }
    const double outcome = value.valueAt(80 * kSecond);
    EXPECT_TRUE(outcome == 0.0 || outcome == 100.0);
}

// A whole-number line can be met exactly: then the contract pays its tie.
TEST(GameTest, AResultOnTheLinePaysTheTie) {
    GameConfig game{.length = 20 * kSecond,
                    .scores = 2.0,
                    .tie = 50.0,
                    .script = {{.at = 5 * kSecond, .home = true, .points = 1},
                               {.at = 9 * kSecond, .home = false, .points = 1}}};
    game.side = GameSide::Over;
    game.line = 2.0;
    Fundamental value{PredictionConfig{.probability = 0.5, .game = game}, 30 * kSecond,
                      Random{1, 0}};
    EXPECT_EQ(value.valueAt(30 * kSecond), 50.0);
}

// With no drift the value is the chance of what happens: of the moments it says 20 to 40, about
// 30% resolve yes, and so on.
TEST(GameTest, TheValueIsCalibrated) {
    std::array<double, 5> valued{};
    std::array<double, 5> resolved{};
    std::array<double, 5> count{};
    const PredictionConfig config{.probability = 0.6, .game = smallGame()};
    for (int path = 0; path < kPaths; ++path) {
        Fundamental value{config, 80 * kSecond, Random{static_cast<std::uint64_t>(path), 3}};
        std::array<double, 4> said{};
        for (std::size_t moment = 0; moment < said.size(); ++moment) {
            said[moment] = value.valueAt(static_cast<Timestamp>(20 + 15 * moment) * kSecond);
        }
        const double outcome = value.valueAt(80 * kSecond);
        for (const double at : said) {
            const auto bin = std::min<std::size_t>(static_cast<std::size_t>(at / 20.0), 4);
            valued[bin] += at;
            resolved[bin] += outcome;
            count[bin] += 1;
        }
    }
    for (std::size_t bin = 0; bin < 5; ++bin) {
        if (count[bin] > 200) {
            EXPECT_NEAR(resolved[bin] / count[bin], valued[bin] / count[bin],
                        4.0 * 50.0 / std::sqrt(count[bin]))
                << "values from " << 20 * bin << " to " << 20 * (bin + 1);
        }
    }
}

// Scores come at the expected rate, worth what they are worth on average.
TEST(GameTest, SidesScoreAtTheExpectedRate) {
    const PredictionConfig config{
        .game = GameConfig{
            .length = 60 * kSecond, .scores = 40.0, .points = {2, 3}, .pointWeights = {3.0, 1.0}}};
    double events = 0.0;
    for (std::uint64_t path = 0; path < 500; ++path) {
        Fundamental value{config, 60 * kSecond, Random{path, 5}};
        static_cast<void>(value.valueAt(60 * kSecond));
        events += static_cast<double>(value.jumps());
    }
    EXPECT_NEAR(events / 500, 40.0, 1.0);
}

// With scoring three times as fast at the end as at the start, the last quarter of the game
// expects (2.5 + 3) / 2 · 1/4 of the 2-unit average rate: 11/32 of the scores, against 5/32 in
// the first quarter.
TEST(GameTest, ScoringCanQuickenThroughTheGame) {
    PredictionConfig config{.game = GameConfig{.length = 40 * kSecond, .scores = 20.0}};
    config.game->lateScoring = 3.0;
    double first = 0.0;
    double last = 0.0;
    for (std::uint64_t path = 0; path < 2'000; ++path) {
        Fundamental value{config, 40 * kSecond, Random{path, 8}};
        static_cast<void>(value.valueAt(10 * kSecond));
        first += static_cast<double>(value.jumps());
        static_cast<void>(value.valueAt(30 * kSecond));
        const auto before = static_cast<double>(value.jumps());
        static_cast<void>(value.valueAt(40 * kSecond));
        last += static_cast<double>(value.jumps()) - before;
    }
    EXPECT_NEAR(first / 2'000, 20.0 * 5.0 / 32.0, 0.1);
    EXPECT_NEAR(last / 2'000, 20.0 * 11.0 / 32.0, 0.15);
}

// The clock stops between periods: nothing scores and the value holds, and after the final
// whistle it is the result.
TEST(GameTest, TheClockStopsAtBreaksAndTheResultHoldsAfterTheEnd) {
    const PredictionConfig config{.game = GameConfig{.length = 40 * kSecond,
                                                     .periods = 2,
                                                     .breakLength = 30 * kSecond,
                                                     .scores = 20.0,
                                                     .tie = 50.0}};
    for (std::uint64_t path = 0; path < 50; ++path) {
        Fundamental value{config, 100 * kSecond, Random{path, 1}};
        const double atBreak = value.valueAt(20 * kSecond);
        const double lead = value.walk();
        EXPECT_EQ(value.valueAt(49 * kSecond), atBreak);
        EXPECT_EQ(value.walk(), lead);
        const double afterGame = value.valueAt(75 * kSecond);
        const double result = value.walk() > 0 ? 100.0 : (value.walk() == 0 ? 50.0 : 0.0);
        EXPECT_EQ(afterGame, result);
        EXPECT_EQ(value.valueAt(99 * kSecond), result);
        EXPECT_EQ(value.valueAt(100 * kSecond), result);
    }
}

// A market that closes before the final whistle stops at the probability then.
TEST(GameTest, AMarketThatClosesEarlyStopsAtTheProbability) {
    for (std::uint64_t path = 0; path < 20; ++path) {
        const PredictionConfig config{.game = smallGame()};
        Fundamental full{config, 80 * kSecond, Random{path, 6}};
        Fundamental cut{config, 40 * kSecond, Random{path, 6}};
        const double then = full.valueAt(40 * kSecond);
        EXPECT_EQ(cut.valueAt(40 * kSecond), then);
        EXPECT_EQ(cut.valueAt(41 * kSecond), then);
    }
}

// A scripted game scores when the script says, worth what it says, and its value moves with it;
// the strength alone is drawn.
TEST(GameTest, AScriptedGameReplaysItsScores) {
    PredictionConfig config{.probability = 0.5, .game = smallGame()};
    config.game->inPlayDrift = 0.5;
    config.game->script = {{.at = 20 * kSecond, .home = false, .points = 1},
                           {.at = 45 * kSecond, .home = true, .points = 2},
                           {.at = 72 * kSecond, .home = true, .points = 1}};
    for (std::uint64_t path = 0; path < 5; ++path) {
        Fundamental value{config, 80 * kSecond, Random{path, 0}};
        const double before = value.valueAt(19 * kSecond);
        EXPECT_EQ(value.walk(), 0.0);
        EXPECT_LT(value.valueAt(20 * kSecond), before);
        EXPECT_EQ(value.walk(), -1.0);
        static_cast<void>(value.valueAt(45 * kSecond));
        EXPECT_EQ(value.walk(), 1.0);
        // After the clock has run out, in time added on, a score still counts.
        EXPECT_EQ(value.valueAt(71 * kSecond), 100.0);
        EXPECT_EQ(value.valueAt(80 * kSecond), 100.0);
        EXPECT_EQ(value.walk(), 2.0);
        EXPECT_EQ(value.jumps(), 3);
    }
}

// A home score lifts the home side's chance, an away score lowers it.
TEST(GameTest, ScoresMoveTheValueTheirWay) {
    int checked = 0;
    for (std::uint64_t path = 0; path < 200 && checked < 20; ++path) {
        Fundamental value{PredictionConfig{.game = smallGame()}, 70 * kSecond, Random{path, 2}};
        double lead = 0.0;
        double before = value.valueAt(10 * kSecond);
        for (Timestamp time = 11 * kSecond; time < 70 * kSecond; time += kSecond) {
            const double now = value.valueAt(time);
            if (value.walk() > lead) {
                EXPECT_GT(now, before);
                ++checked;
            } else if (value.walk() < lead) {
                EXPECT_LT(now, before);
                ++checked;
            }
            lead = value.walk();
            before = now;
        }
    }
    EXPECT_GE(checked, 20);
}

// A trader who sees the game late sees what was, step for step.
TEST(GameTest, ReadingLateGivesWhatWasThen) {
    const PredictionConfig config{.game = GameConfig{.start = 20 * kSecond,
                                                     .length = 60 * kSecond,
                                                     .scores = 10.0,
                                                     .pregameDrift = 0.5,
                                                     .inPlayDrift = 0.3,
                                                     .news = 3.0,
                                                     .newsSize = 0.3}};
    Fundamental onTime{config, 90 * kSecond, Random{4, 0}};
    Fundamental late{config, 90 * kSecond, Random{4, 0}};
    late.remember(10 * kSecond);
    for (Timestamp time = 0; time < 90 * kSecond; time += kSecond) {
        const double then = onTime.valueAt(time);
        static_cast<void>(late.valueAt(time + 5 * kSecond));
        EXPECT_EQ(late.valueAt(time), then) << "at " << time / kSecond << "s";
    }
}

TEST(GameTest, SettingsAreChecked) {
    const auto with = [](auto change) {
        PredictionConfig config{.game = smallGame()};
        change(*config.game);
        return config;
    };
    for (const PredictionConfig& config :
         {with([](GameConfig& g) { g.length = 0; }),
          with([](GameConfig& g) { g.start = -kSecond; }),
          with([](GameConfig& g) { g.periods = 0; }), with([](GameConfig& g) { g.scores = 0.0; }),
          with([](GameConfig& g) { g.scores = 600.0; }), with([](GameConfig& g) { g.points = {}; }),
          with([](GameConfig& g) { g.pointWeights = {1.0, 2.0}; }),
          with([](GameConfig& g) { g.points = {0}; }), with([](GameConfig& g) { g.tie = 101.0; }),
          with([](GameConfig& g) { g.pregameDrift = -1.0; }),
          with([](GameConfig& g) { g.lateScoring = 0.0; }),
          with([](GameConfig& g) { g.script = {{.at = 30 * kSecond}, {.at = 20 * kSecond}}; }),
          with([](GameConfig& g) { g.line = 1.5; }), // a line on a win
          PredictionConfig{.newsRate = 0.1, .game = smallGame()},
          // With 0.1 scores expected, the home side wins at most about one time in ten.
          PredictionConfig{.probability = 0.5,
                           .game = GameConfig{.length = 60 * kSecond, .scores = 0.1}}}) {
        EXPECT_THROW(Fundamental(config, 80 * kSecond, Random{1, 0}), std::invalid_argument);
        EXPECT_THROW(Fundamental::check(config, 80 * kSecond), std::invalid_argument);
    }
}

} // namespace
} // namespace crowdbook

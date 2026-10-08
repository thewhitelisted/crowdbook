#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <utility>

#include <gtest/gtest.h>

#include "crowdbook/math.hpp"
#include "crowdbook/random.hpp"

namespace crowdbook {
namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr int kDraws = 200'000;

// How many representable doubles lie between a and b: 0 when they are equal, both NaN, or the
// same infinity.
std::uint64_t ulps(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) {
        return 0;
    }
    if (std::isnan(a) || std::isnan(b)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    // Map the bits to integers that order the same way as the doubles.
    const auto ordered = [](double x) {
        const auto bits = std::bit_cast<std::int64_t>(x);
        return bits < 0 ? std::numeric_limits<std::int64_t>::min() - bits : bits;
    };
    const std::int64_t difference = ordered(a) - ordered(b);
    return difference < 0 ? static_cast<std::uint64_t>(-difference)
                          : static_cast<std::uint64_t>(difference);
}

// The largest difference, in ulps, between f and reference over the inputs.
template <typename Ours, typename Reference>
std::uint64_t worst(const std::vector<double>& inputs, Ours ours, Reference reference) {
    std::uint64_t largest = 0;
    for (const double x : inputs) {
        largest = std::max(largest, ulps(ours(x), reference(x)));
    }
    return largest;
}

std::vector<double> uniform(double low, double high, std::uint64_t stream) {
    Random random{7, stream};
    std::vector<double> values;
    for (int i = 0; i < kDraws; ++i) {
        values.push_back(low + (high - low) * random.uniform());
    }
    return values;
}

// Positive doubles spread evenly over every exponent, subnormals included.
std::vector<double> everyMagnitude(std::uint64_t stream) {
    Random random{7, stream};
    std::vector<double> values;
    while (values.size() < kDraws) {
        const double x = std::bit_cast<double>(random.uniformInt(1, 0x7fefffffffffffff));
        values.push_back(x);
    }
    return values;
}

TEST(MathTest, FloatingPointIsEvaluatedAsWritten) {
    // (1 + 2^-30)^2 = 1 + 2^-29 + 2^-60, which rounds to 1 + 2^-29. A fused multiply-add would
    // keep the 2^-60 in a * a - product; the build turns fusing off so every platform agrees.
    volatile double input = 1.0 + 0x1.0p-30;
    const double a = input;
    const double product = a * a;
    EXPECT_EQ(a * a - product, 0.0);
}

TEST(MathTest, ExpMatchesTheStandardLibrary) {
    const auto ours = [](double x) { return math::exp(x); };
    const auto theirs = [](double x) { return std::exp(x); };
    EXPECT_LE(worst(uniform(-745.0, 709.78, 1), ours, theirs), 2U);
    EXPECT_LE(worst(uniform(-1.0, 1.0, 2), ours, theirs), 1U);
    EXPECT_LE(worst(uniform(-1e-9, 1e-9, 3), ours, theirs), 1U);
    EXPECT_EQ(math::exp(0.0), 1.0);
    EXPECT_EQ(math::exp(-kInfinity), 0.0);
    EXPECT_EQ(math::exp(kInfinity), kInfinity);
    EXPECT_EQ(math::exp(710.0), kInfinity);
    EXPECT_EQ(math::exp(-746.0), 0.0);
    EXPECT_TRUE(std::isnan(math::exp(std::nan(""))));
}

TEST(MathTest, Exp2MatchesTheStandardLibraryAndIsExactForWholeNumbers) {
    const auto ours = [](double x) { return math::exp2(x); };
    const auto theirs = [](double x) { return std::exp2(x); };
    EXPECT_LE(worst(uniform(-1074.0, 1023.9, 4), ours, theirs), 2U);
    EXPECT_LE(worst(uniform(-1.0, 1.0, 5), ours, theirs), 1U);
    for (int n = -1074; n <= 1023; ++n) {
        ASSERT_EQ(math::exp2(n), std::ldexp(1.0, n)) << n;
    }
    EXPECT_EQ(math::exp2(1024.0), kInfinity);
    EXPECT_EQ(math::exp2(-1080.0), 0.0);
}

TEST(MathTest, LogMatchesTheStandardLibrary) {
    const auto ours = [](double x) { return math::log(x); };
    const auto theirs = [](double x) { return std::log(x); };
    EXPECT_LE(worst(everyMagnitude(6), ours, theirs), 1U);
    EXPECT_LE(worst(uniform(0.5, 2.0, 7), ours, theirs), 1U);
    EXPECT_LE(worst(uniform(1.0 - 1e-6, 1.0 + 1e-6, 8), ours, theirs), 1U);
    EXPECT_EQ(math::log(1.0), 0.0);
    EXPECT_EQ(math::log(0.0), -kInfinity);
    EXPECT_EQ(math::log(-0.0), -kInfinity);
    EXPECT_EQ(math::log(kInfinity), kInfinity);
    EXPECT_TRUE(std::isnan(math::log(-1.0)));
    EXPECT_EQ(math::log(std::numeric_limits<double>::denorm_min()),
              std::log(std::numeric_limits<double>::denorm_min()));
}

TEST(MathTest, PowMatchesTheStandardLibrary) {
    Random random{7, 9};
    std::uint64_t largest = 0;
    for (int i = 0; i < kDraws; ++i) {
        const double x = math::exp(-46.0 + 92.0 * random.uniform()); // 1e-20 to 1e20
        const double y = -40.0 + 80.0 * random.uniform();
        const double expected = std::pow(x, y);
        if (expected == 0.0 || std::isinf(expected) ||
            expected < std::numeric_limits<double>::min()) {
            continue;
        }
        largest = std::max(largest, ulps(math::pow(x, y), expected));
    }
    EXPECT_LE(largest, 2U);

    // The cases the agents use: ratios near 1 to a modest power, and draws to -1 / tail.
    const auto nearOne = uniform(0.5, 2.0, 10);
    const auto powers = uniform(-3.0, 3.0, 11);
    largest = 0;
    for (std::size_t i = 0; i < nearOne.size(); ++i) {
        largest = std::max(largest, ulps(math::pow(nearOne[i], powers[i]),
                                         std::pow(nearOne[i], powers[i])));
    }
    EXPECT_LE(largest, 1U);
}

TEST(MathTest, PowHasTheStandardSpecialCases) {
    const double nan = std::nan("");
    const std::vector<std::pair<double, double>> cases = {
        {2.0, 10.0},  {-2.0, 3.0},  {-2.0, 2.0},   {-2.0, 0.5},       {0.0, 3.0},
        {-0.0, 3.0},  {-0.0, 2.0},  {0.0, -1.0},   {-0.0, -1.0},      {-0.0, -2.0},
        {nan, 0.0},   {1.0, nan},   {nan, 1.0},    {2.0, nan},        {-1.0, kInfinity},
        {0.5, kInfinity}, {0.5, -kInfinity}, {2.0, kInfinity}, {2.0, -kInfinity},
        {kInfinity, 2.0}, {kInfinity, -2.0}, {-kInfinity, 3.0}, {-kInfinity, 2.0},
        {-kInfinity, -3.0}, {-kInfinity, -2.0}, {3.0, 1.0}, {1e300, 2.0}, {1e-300, 2.0},
    };
    for (const auto& [x, y] : cases) {
        const double ours = math::pow(x, y);
        const double theirs = std::pow(x, y);
        EXPECT_EQ(ulps(ours, theirs), 0U) << x << "^" << y;
        if (!std::isnan(theirs)) { // a NaN's sign means nothing, and libraries differ on it
            EXPECT_EQ(std::signbit(ours), std::signbit(theirs)) << x << "^" << y;
        }
    }
}

// Φ against values worked out to many digits, in the middle and far into the tails, where what
// matters is that it is accurate in absolute terms: to about 1e-15, a thousand-billionth of a cent
// in a prediction market's price.
TEST(MathTest, NormalCdfMatchesKnownValues) {
    const std::pair<double, double> known[] = {
        {0.0, 0.5},
        {1.0, 0.84134474606854292578},
        {-1.0, 0.15865525393145707422},
        {1.959963984540054, 0.975},
        {2.5, 0.99379033467422384602},
        {-3.0, 0.0013498980316300945267},
        {5.0, 0.99999971334842807106},
        {-5.0, 2.8665157187919391167e-7},
        {-8.0, 6.2209605742717841235e-16},
    };
    for (const auto& [x, expected] : known) {
        EXPECT_NEAR(math::normalCdf(x), expected, 1e-15) << x;
    }
    for (double x = -9.5; x <= 9.5; x += 0.37) {
        EXPECT_NEAR(math::normalCdf(x) + math::normalCdf(-x), 1.0, 2e-15) << x;
        EXPECT_LE(math::normalCdf(x), math::normalCdf(x + 0.01) + 1e-15) << x;
    }
    EXPECT_EQ(math::normalCdf(-11.0), 0.0);
    EXPECT_EQ(math::normalCdf(11.0), 1.0);
}

} // namespace
} // namespace crowdbook

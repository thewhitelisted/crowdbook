#include "crowdbook/math.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "math_tables.hpp"

namespace crowdbook::math {

namespace {

using detail::kExp2Steps;
using detail::kLn2;
using detail::kLn2Hi;
using detail::kLn2Lo;
using detail::kLn2Rest;
using detail::kLogFirst;
using detail::kLogSteps;
using detail::kStepHi;
using detail::kStepLo;
using detail::kStepsPerLn2;

constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr double kNotANumber = std::numeric_limits<double>::quiet_NaN();
constexpr double kSqrt2 = 0x1.6a09e667f3bcdp+0;
// Beyond these, e^x is certain to overflow or to round to zero.
constexpr double kLargestExponent = 710.0;
constexpr double kSmallestExponent = -746.0;

// A number held as the unevaluated sum hi + lo, to carry more precision than one double.
struct Pair {
    double hi;
    double lo;
};

// a + b exactly, as the rounded sum and its error (Knuth).
Pair twoSum(double a, double b) {
    const double sum = a + b;
    const double bPart = sum - a;
    return {sum, (a - (sum - bPart)) + (b - bPart)};
}

// a + b exactly, when |a| >= |b| or a is 0 (Dekker).
Pair fastTwoSum(double a, double b) {
    const double sum = a + b;
    return {sum, b - (sum - a)};
}

// a in two halves of at most 26 significant bits each, whose products are exact (Veltkamp).
Pair split(double a) {
    constexpr double kSplitter = 0x1.0p27 + 1.0;
    const double scaled = kSplitter * a;
    const double hi = scaled - (scaled - a);
    return {hi, a - hi};
}

// a × b exactly, as the rounded product and its error (Dekker), for |a| and |b| below 2^995.
Pair twoProduct(double a, double b) {
    const double product = a * b;
    const auto [aHi, aLo] = split(a);
    const auto [bHi, bLo] = split(b);
    return {product, ((aHi * bHi - product) + aHi * bLo + aLo * bHi) + aLo * bLo};
}

// 2^n, for n from -1022 to 1023.
double powerOfTwo(int n) {
    return std::bit_cast<double>(static_cast<std::uint64_t>(n + 1023) << 52);
}

// e^(rHi + rLo) × 2^(k / 64), for a whole number k and |rHi + rLo| not much over ln 2 / 128.
double expCore(double k, double rHi, double rLo) {
    const auto [r, rest] = twoSum(rHi, rLo);
    // e^r - 1 = r + r^2/2! + ... + r^7/7!, with the first term kept apart; the next term is below
    // 2^-80. The pair's second part adds rest × e^r, to first order in rest.
    const double tail =
        r * r *
        (1.0 / 2.0 +
         r * (1.0 / 6.0 +
              r * (1.0 / 24.0 + r * (1.0 / 120.0 + r * (1.0 / 720.0 + r * (1.0 / 5040.0))))));
    const double small = rest + rest * r + tail;

    const int whole = static_cast<int>(k);
    const int j = whole & 63;
    const int q = (whole - j) / 64;
    const double stepHi = kExp2Steps[static_cast<std::size_t>(j)][0];
    const double stepLo = kExp2Steps[static_cast<std::size_t>(j)][1];
    // 2^(j/64) × e^(r + rest), with only the final addition rounding at full size.
    const double value = stepHi + (stepHi * r + (stepLo + stepHi * small + stepLo * (r + small)));
    // Scaled in two steps, each by a power of two in range, so only a subnormal result rounds.
    const int half = q / 2;
    return value * powerOfTwo(half) * powerOfTwo(q - half);
}

// e^(hi + lo), for lo much smaller than hi.
double expPair(double hi, double lo) {
    if (hi > kLargestExponent) {
        return kInfinity;
    }
    if (hi < kSmallestExponent) {
        return 0.0;
    }
    const double k = std::round(hi * kStepsPerLn2);
    // k × kStepHi is exact, and so is taking it from hi, which is that close to it.
    return expCore(k, hi - k * kStepHi, lo - k * kStepLo);
}

// The natural logarithm of a positive, finite x, to about 2^-68 relative.
Pair logPair(double x) {
    std::uint64_t bits = std::bit_cast<std::uint64_t>(x);
    int exponent = 0;
    if ((bits >> 52) == 0) { // subnormal: scale it up first, exactly
        bits = std::bit_cast<std::uint64_t>(x * 0x1.0p54);
        exponent = -54;
    }
    exponent += static_cast<int>(bits >> 52) - 1023;
    constexpr std::uint64_t kFraction = (std::uint64_t{1} << 52) - 1;
    double mantissa = std::bit_cast<double>((bits & kFraction) | (std::uint64_t{1023} << 52));
    if (mantissa > kSqrt2) { // keep it within [1/sqrt(2), sqrt(2)], around 1
        mantissa *= 0.5;
        ++exponent;
    }

    // x = 2^exponent × mantissa, and mantissa = (1 + r) / inverse for the nearest tabulated
    // inverse, so log x = exponent × ln 2 - log(inverse) + log(1 + r), with |r| below 2^-7.
    const int i = static_cast<int>(std::round((mantissa - 1.0) * 128.0));
    const auto& step = kLogSteps[static_cast<std::size_t>(i - kLogFirst)];
    const auto [product, productError] = twoProduct(mantissa, step[0]);
    const double r = product - 1.0; // exact, as product is within a factor of 2 of 1
    // log(1 + r) - r = -r^2/2 + r^3/3 - ... - r^10/10; the next term is below 2^-78. The product's
    // error adds productError / (1 + r), to second order.
    const double series =
        r * r *
        (-1.0 / 2.0 +
         r * (1.0 / 3.0 +
              r * (-1.0 / 4.0 +
                   r * (1.0 / 5.0 +
                        r * (-1.0 / 6.0 +
                             r * (1.0 / 7.0 +
                                  r * (-1.0 / 8.0 + r * (1.0 / 9.0 + r * (-1.0 / 10.0)))))))));
    const double scale = static_cast<double>(exponent);
    const Pair first = twoSum(scale * kLn2Hi, step[1]); // the product is exact
    const Pair second = twoSum(first.hi, r);
    const double lo = first.lo + second.lo + scale * kLn2Lo + step[2] + productError -
                      productError * r + series;
    return fastTwoSum(second.hi, lo);
}

bool isInteger(double y) { return std::trunc(y) == y; }

bool isOddInteger(double y) { return isInteger(y) && std::fmod(y, 2.0) != 0.0; }

} // namespace

double exp(double x) noexcept {
    if (std::isnan(x)) {
        return x;
    }
    return expPair(x, 0.0);
}

double exp2(double x) noexcept {
    if (std::isnan(x)) {
        return x;
    }
    if (x > 1025.0) {
        return kInfinity;
    }
    if (x < -1080.0) {
        return 0.0;
    }
    // x = k/64 + f exactly, with |f| at most 1/128, and 2^f = e^(f ln 2).
    const double k = std::round(x * 64.0);
    const double f = x - k / 64.0;
    const auto [rHi, rLo] = twoProduct(f, kLn2);
    return expCore(k, rHi, rLo + f * kLn2Rest);
}

double log(double x) noexcept {
    if (std::isnan(x)) {
        return x;
    }
    if (x == 0.0) {
        return -kInfinity;
    }
    if (x < 0.0) {
        return kNotANumber;
    }
    if (x == kInfinity) {
        return x;
    }
    return logPair(x).hi;
}

double pow(double x, double y) noexcept {
    if (y == 0.0 || x == 1.0) {
        return 1.0;
    }
    if (std::isnan(x) || std::isnan(y)) {
        return kNotANumber;
    }
    if (y == 1.0) {
        return x;
    }
    if (std::isinf(y)) {
        const double size = std::fabs(x);
        if (size == 1.0) {
            return 1.0;
        }
        return (size < 1.0) == (y > 0.0) ? 0.0 : kInfinity;
    }
    const bool odd = isOddInteger(y);
    if (x == 0.0) {
        if (y > 0.0) {
            return odd ? x : 0.0;
        }
        return odd ? std::copysign(kInfinity, x) : kInfinity;
    }
    if (std::isinf(x)) {
        if (x > 0.0) {
            return y > 0.0 ? kInfinity : 0.0;
        }
        if (y > 0.0) {
            return odd ? -kInfinity : kInfinity;
        }
        return odd ? -0.0 : 0.0;
    }
    if (x < 0.0 && !isInteger(y)) {
        return kNotANumber;
    }

    // e^(y log|x|), with y log|x| carried as a pair so the result keeps full precision.
    const auto [logHi, logLo] = logPair(std::fabs(x));
    const double estimate = y * logHi;
    double result = 0.0;
    if (std::fabs(estimate) > 1500.0) {
        result = estimate > 0.0 ? kInfinity : 0.0;
    } else {
        const auto [zHi, zError] = twoProduct(y, logHi);
        const auto [hi, lo] = fastTwoSum(zHi, zError + y * logLo);
        result = expPair(hi, lo);
    }
    return x < 0.0 && odd ? -result : result;
}

} // namespace crowdbook::math

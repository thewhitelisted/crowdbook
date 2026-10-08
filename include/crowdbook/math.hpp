#pragma once

namespace crowdbook::math {

// Elementary functions that give the same result on every platform, so that a seeded run is the
// same everywhere. The standard library's versions differ between implementations in the last
// bit, which is enough to send two runs down different paths. These use only addition,
// subtraction, multiplication, division and exact operations on a double's bits, evaluated in a
// fixed order, with tables computed once to 60 digits (tools/math_tables.py). They are accurate to
// within about one unit in the last place. Agents should use them, never std::exp and the like,
// for anything that affects what they do.
//
// They need the compiler to evaluate floating-point expressions as written: no fused
// multiply-adds (-ffp-contract=off) and no fast-math.

// e^x.
[[nodiscard]] double exp(double x) noexcept;
// 2^x.
[[nodiscard]] double exp2(double x) noexcept;
// The natural logarithm: -infinity at 0, NaN below.
[[nodiscard]] double log(double x) noexcept;
// x^y, with the special cases of the C standard's pow.
[[nodiscard]] double pow(double x, double y) noexcept;
// The standard normal distribution function Φ(x): the probability that a standard normal draw is
// at most x. Accurate to about 1e-16 everywhere; exactly 0 below -10 and 1 above 10.
[[nodiscard]] double normalCdf(double x) noexcept;

} // namespace crowdbook::math

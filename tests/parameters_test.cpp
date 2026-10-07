#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/parameters.hpp"

namespace crowdbook {
namespace {

TEST(ParseDurationTest, ReadsEveryUnitExactly) {
    EXPECT_EQ(parseDuration("250ns"), 250);
    EXPECT_EQ(parseDuration("50us"), 50 * kMicrosecond);
    EXPECT_EQ(parseDuration("1.5ms"), 1'500'000);
    EXPECT_EQ(parseDuration("2s"), 2 * kSecond);
    EXPECT_EQ(parseDuration("0s"), 0);
    EXPECT_EQ(parseDuration("1.25us"), 1'250);
    EXPECT_EQ(parseDuration("0.000000001s"), 1);
    EXPECT_EQ(parseDuration("3.0ns"), 3);
    EXPECT_EQ(parseDuration("999999999.999999999s"), kMaxDuration - 1);
}

TEST(ParseDurationTest, RejectsMalformedText) {
    for (const char* text : {"", "5", "ms", "-1ms", "1..5ms", "1.ms", ".5ms", "5 ms", "5min",
                             "1.5ns", "1.0000005us", "99999999999999999999s", "1000000000s",
                             "1000000000000000000ns"}) {
        EXPECT_THROW(static_cast<void>(parseDuration(text)), std::invalid_argument) << text;
    }
}

TEST(ParametersTest, ReturnsStoredValuesOrFallbacks) {
    Parameters parameters;
    parameters.set("enabled", true);
    parameters.set("size", std::int64_t{5});
    parameters.set("rate", 2.5);
    parameters.set("label", std::string{"noise"});
    parameters.set("interval", std::string{"20ms"});

    EXPECT_TRUE(parameters.flag("enabled", false));
    EXPECT_EQ(parameters.integer("size", 1), 5);
    EXPECT_EQ(parameters.number("rate", 0.0), 2.5);
    EXPECT_EQ(parameters.number("size", 0.0), 5.0); // whole numbers count as numbers
    EXPECT_EQ(parameters.text("label", "x"), "noise");
    EXPECT_EQ(parameters.duration("interval", 0), 20 * kMillisecond);

    EXPECT_FALSE(parameters.flag("missing", false));
    EXPECT_EQ(parameters.integer("missing", 7), 7);
    EXPECT_EQ(parameters.duration("missing", kSecond), kSecond);
}

TEST(ParametersTest, WrongTypesNameTheParameter) {
    Parameters parameters;
    parameters.set("size", std::string{"big"});
    parameters.set("interval", std::int64_t{20});
    parameters.set("delay", std::string{"20 minutes"});

    try {
        static_cast<void>(parameters.integer("size", 1));
        FAIL() << "expected a type error";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string{error.what()}.find("'size'"), std::string::npos) << error.what();
    }
    EXPECT_THROW(static_cast<void>(parameters.number("size", 1.0)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(parameters.flag("size", false)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(parameters.duration("interval", 0)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(parameters.duration("delay", 0)), std::invalid_argument);
}

TEST(ParametersTest, ReportsNamesThatWereNeverRead) {
    Parameters parameters;
    parameters.set("limit_rate", 1.0);
    parameters.set("limt_rate", 2.0); // a typo the agent will never read
    parameters.set("max_size", std::int64_t{3});

    static_cast<void>(parameters.number("limit_rate", 0.0));
    static_cast<void>(parameters.integer("max_size", 0));
    static_cast<void>(parameters.integer("min_size", 1)); // absent names are not reported
    EXPECT_EQ(parameters.unusedNames(), std::vector<std::string>{"limt_rate"});
}

} // namespace
} // namespace crowdbook

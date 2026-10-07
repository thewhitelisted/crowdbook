#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

#include <gtest/gtest.h>

#include "json.hpp"

namespace crowdbook::json {
namespace {

void parseOnly(std::string_view text) {
    static_cast<void>(parse(text));
}

std::int64_t integer(std::string_view text) {
    return std::get<std::int64_t>(parse(text).data);
}

TEST(Json, ParsesEveryKindOfValue) {
    const Value value =
        parse(R"( {"a": 1, "b": [true, false, null], "c": "x\"y", "d": {}, "e": [] } )");
    const auto& object = std::get<Object>(value.data);
    ASSERT_EQ(object.size(), 5U);
    EXPECT_EQ(object[0].key, "a");
    EXPECT_EQ(std::get<std::int64_t>(object[0].value.data), 1);
    const auto& list = std::get<Array>(object[1].value.data);
    ASSERT_EQ(list.size(), 3U);
    EXPECT_TRUE(std::get<bool>(list[0].data));
    EXPECT_FALSE(std::get<bool>(list[1].data));
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(list[2].data));
    EXPECT_EQ(std::get<std::string>(object[2].value.data), "x\"y");
    EXPECT_TRUE(std::get<Object>(object[3].value.data).empty());
    EXPECT_TRUE(std::get<Array>(object[4].value.data).empty());
}

TEST(Json, IntegersCoverTheWholeInt64Range) {
    EXPECT_EQ(integer("0"), 0);
    EXPECT_EQ(integer("-0"), 0);
    EXPECT_EQ(integer("-17"), -17);
    EXPECT_EQ(integer("9223372036854775807"), std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(integer("-9223372036854775808"), std::numeric_limits<std::int64_t>::min());
    EXPECT_THROW(parseOnly("9223372036854775808"), ParseError);
    EXPECT_THROW(parseOnly("-9223372036854775809"), ParseError);
    EXPECT_THROW(parseOnly("99999999999999999999999"), ParseError);
}

TEST(Json, NumbersMustBeWholeAndPlain) {
    for (const std::string_view text :
         {"1.5", "1e3", "1E3", "01", "-", "+1", "- 1", ".5", "0x10"}) {
        EXPECT_THROW(parseOnly(text), ParseError) << text;
    }
}

TEST(Json, StringsAreAsciiWithStandardEscapes) {
    EXPECT_EQ(std::get<std::string>(parse(R"("\"\\\/\b\f\n\r\t")").data), "\"\\/\b\f\n\r\t");
    EXPECT_EQ(std::get<std::string>(parse(R"("\u0041\u007f\u0000")").data),
              std::string("A\x7f\0", 3));
    EXPECT_THROW(parseOnly(R"("\u0080")"), ParseError);
    EXPECT_THROW(parseOnly(R"("\u00e9")"), ParseError);
    EXPECT_THROW(parseOnly(R"("\u12")"), ParseError);
    EXPECT_THROW(parseOnly(R"("\x41")"), ParseError);
    EXPECT_THROW(parseOnly("\"caf\xc3\xa9\""), ParseError);
    EXPECT_THROW(parseOnly("\"a\tb\""), ParseError);
    EXPECT_THROW(parseOnly("\"unfinished"), ParseError);
    EXPECT_THROW(parseOnly("\"unfinished\\"), ParseError);
}

TEST(Json, RejectsMalformedStructure) {
    for (const std::string_view text :
         {"", " ", "{", "}", "[", "{\"a\":1,}", "[1,]", "{\"a\" 1}", "{a:1}", "{\"a\":1}x",
          "[1 2]", "tru", "nul", "falsey", "{\"a\":1,\"a\":2}", "1 2"}) {
        EXPECT_THROW(parseOnly(text), ParseError) << text;
    }
}

TEST(Json, LimitsNesting) {
    std::string deepest;
    for (std::size_t i = 0; i < kMaxDepth; ++i) {
        deepest += '[';
    }
    deepest += std::string(kMaxDepth, ']');
    EXPECT_NO_THROW(parseOnly(deepest));
    EXPECT_THROW(parseOnly("[" + deepest + "]"), ParseError);
    EXPECT_THROW(parseOnly("{\"a\":" + deepest + "}"), ParseError);
}

TEST(Json, WrittenStringsParseBackToThemselves) {
    std::string every;
    for (int c = 0; c < 0x80; ++c) {
        every += static_cast<char>(c);
    }
    std::string out;
    appendString(out, every);
    EXPECT_EQ(std::get<std::string>(parse(out).data), every);
}

TEST(Json, WrittenStringsReplaceBytesOutsideAscii) {
    std::string out;
    appendString(out, "caf\xc3\xa9");
    EXPECT_EQ(out, "\"caf??\"");
    EXPECT_EQ(std::get<std::string>(parse(out).data), "caf??");
}

} // namespace
} // namespace crowdbook::json

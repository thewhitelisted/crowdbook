#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The part of JSON the protocol uses: objects, arrays, ASCII strings, whole numbers that fit an
// int64, true, false and null. Parsing is strict, since its input comes from the network.
namespace crowdbook::json {

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Value;
struct Member;
using Array = std::vector<Value>;
using Object = std::vector<Member>; // in the order written; keys are unique

// Neither struct has default member initializers: one would instantiate the variant's
// constructor, and with it the destructor of std::vector<Member>, while Member is incomplete. A
// default-constructed Value holds null.
struct Value {
    std::variant<std::nullptr_t, bool, std::int64_t, std::string, Array, Object> data;
};

struct Member {
    std::string key;
    Value value;
};

// Arrays and objects may nest at most this deep.
inline constexpr std::size_t kMaxDepth = 8;

// Parses one JSON value, with nothing but whitespace around it. Throws ParseError for anything
// else, including fractions and exponents, numbers outside int64, characters outside ASCII,
// control characters in strings, \u escapes beyond \u007f, repeated keys and nesting deeper than
// kMaxDepth.
[[nodiscard]] Value parse(std::string_view text);

// Appends `text` as a JSON string, quoted and escaped. Bytes outside ASCII become '?', so the
// output is always something parse accepts.
void appendString(std::string& out, std::string_view text);

} // namespace crowdbook::json

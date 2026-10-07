#include "json.hpp"

#include <format>
#include <limits>
#include <utility>

namespace crowdbook::json {

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) noexcept : text_(text) {}

    Value document() {
        Value value = parseValue(0);
        skipWhitespace();
        if (position_ != text_.size()) {
            fail("unexpected text after the value");
        }
        return value;
    }

private:
    [[noreturn]] void fail(std::string_view message) const {
        throw ParseError(std::format("{} at byte {}", message, position_));
    }

    [[nodiscard]] bool atEnd() const noexcept { return position_ >= text_.size(); }
    [[nodiscard]] char peek() const noexcept { return text_[position_]; }

    void skipWhitespace() noexcept {
        while (!atEnd() &&
               (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
            ++position_;
        }
    }

    void expect(char c) {
        if (atEnd() || peek() != c) {
            fail(std::format("expected '{}'", c));
        }
        ++position_;
    }

    void literal(std::string_view word) {
        if (text_.substr(position_, word.size()) != word) {
            fail("expected a value");
        }
        position_ += word.size();
    }

    Value parseValue(std::size_t depth) {
        skipWhitespace();
        if (atEnd()) {
            fail("expected a value");
        }
        switch (peek()) {
        case '{':
            return {parseObject(depth + 1)};
        case '[':
            return {parseArray(depth + 1)};
        case '"':
            return {parseString()};
        case 't':
            literal("true");
            return {true};
        case 'f':
            literal("false");
            return {false};
        case 'n':
            literal("null");
            return {nullptr};
        default:
            return {parseInteger()};
        }
    }

    Object parseObject(std::size_t depth) {
        if (depth > kMaxDepth) {
            fail(std::format("nested more than {} deep", kMaxDepth));
        }
        expect('{');
        Object object;
        skipWhitespace();
        if (!atEnd() && peek() == '}') {
            ++position_;
            return object;
        }
        while (true) {
            skipWhitespace();
            if (atEnd() || peek() != '"') {
                fail("expected a key in quotes");
            }
            std::string key = parseString();
            for (const Member& member : object) {
                if (member.key == key) {
                    fail(std::format("repeated key '{}'", key));
                }
            }
            skipWhitespace();
            expect(':');
            Value value = parseValue(depth);
            object.push_back({.key = std::move(key), .value = std::move(value)});
            skipWhitespace();
            if (!atEnd() && peek() == ',') {
                ++position_;
                continue;
            }
            expect('}');
            return object;
        }
    }

    Array parseArray(std::size_t depth) {
        if (depth > kMaxDepth) {
            fail(std::format("nested more than {} deep", kMaxDepth));
        }
        expect('[');
        Array array;
        skipWhitespace();
        if (!atEnd() && peek() == ']') {
            ++position_;
            return array;
        }
        while (true) {
            array.push_back(parseValue(depth));
            skipWhitespace();
            if (!atEnd() && peek() == ',') {
                ++position_;
                continue;
            }
            expect(']');
            return array;
        }
    }

    unsigned hexDigit() {
        if (atEnd()) {
            fail("unfinished \\u escape");
        }
        const char c = peek();
        ++position_;
        if (c >= '0' && c <= '9') {
            return static_cast<unsigned>(c - '0');
        }
        if (c >= 'a' && c <= 'f') {
            return static_cast<unsigned>(c - 'a' + 10);
        }
        if (c >= 'A' && c <= 'F') {
            return static_cast<unsigned>(c - 'A' + 10);
        }
        fail("bad hex digit in a \\u escape");
    }

    std::string parseString() {
        expect('"');
        std::string text;
        while (true) {
            if (atEnd()) {
                fail("unfinished string");
            }
            const auto byte = static_cast<unsigned char>(peek());
            ++position_;
            if (byte == '"') {
                return text;
            }
            if (byte >= 0x80) {
                fail("a character outside ASCII");
            }
            if (byte < 0x20) {
                fail("a control character in a string");
            }
            if (byte != '\\') {
                text += static_cast<char>(byte);
                continue;
            }
            if (atEnd()) {
                fail("unfinished escape");
            }
            const char escape = peek();
            ++position_;
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                text += escape;
                break;
            case 'b':
                text += '\b';
                break;
            case 'f':
                text += '\f';
                break;
            case 'n':
                text += '\n';
                break;
            case 'r':
                text += '\r';
                break;
            case 't':
                text += '\t';
                break;
            case 'u': {
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    code = code * 16 + hexDigit();
                }
                if (code >= 0x80) {
                    fail("a \\u escape outside ASCII");
                }
                text += static_cast<char>(code);
                break;
            }
            default:
                fail("unknown escape");
            }
        }
    }

    std::int64_t parseInteger() {
        const bool negative = peek() == '-';
        if (negative) {
            ++position_;
        }
        if (atEnd() || peek() < '0' || peek() > '9') {
            fail("expected a value");
        }
        if (peek() == '0' && position_ + 1 < text_.size() && text_[position_ + 1] >= '0' &&
            text_[position_ + 1] <= '9') {
            fail("a number with a leading zero");
        }
        // Accumulated as a negative number, whose range reaches one further than the positive.
        constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
        std::int64_t value = 0;
        while (!atEnd() && peek() >= '0' && peek() <= '9') {
            const std::int64_t digit = peek() - '0';
            if (value < (kMin + digit) / 10) {
                fail("a number outside the 64-bit range");
            }
            value = value * 10 - digit;
            ++position_;
        }
        if (!atEnd() && (peek() == '.' || peek() == 'e' || peek() == 'E')) {
            fail("numbers must be whole, without fractions or exponents");
        }
        if (negative) {
            return value;
        }
        if (value == kMin) {
            fail("a number outside the 64-bit range");
        }
        return -value;
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

} // namespace

Value parse(std::string_view text) {
    return Parser{text}.document();
}

void appendString(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte >= 0x80) {
                out += '?';
            } else if (byte < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(byte));
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

} // namespace crowdbook::json

#include "crowdbook/parameters.hpp"

#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

namespace crowdbook {

namespace {

constexpr Duration kMaxDuration = std::numeric_limits<Duration>::max();

[[noreturn]] void throwNotADuration(std::string_view text, std::string_view why) {
    throw std::invalid_argument(
        std::format("'{}' is not a valid duration such as \"1.5ms\": {}", text, why));
}

} // namespace

Duration parseDuration(std::string_view text) {
    const auto unitStart = text.find_first_not_of("0123456789.");
    if (unitStart == 0 || unitStart == std::string_view::npos) {
        throwNotADuration(text, "it needs a number followed by ns, us, ms or s");
    }
    const std::string_view unit = text.substr(unitStart);
    Duration scale = 0;
    if (unit == "ns") {
        scale = 1;
    } else if (unit == "us") {
        scale = kMicrosecond;
    } else if (unit == "ms") {
        scale = kMillisecond;
    } else if (unit == "s") {
        scale = kSecond;
    } else {
        throwNotADuration(text, "the unit must be ns, us, ms or s");
    }

    // Integer arithmetic only, so "1.5ms" is exactly 1'500'000 nanoseconds.
    const std::string_view number = text.substr(0, unitStart);
    const auto point = number.find('.');
    const std::string_view whole = number.substr(0, point);
    const std::string_view fraction =
        point == std::string_view::npos ? std::string_view{} : number.substr(point + 1);
    if (whole.empty() || (point != std::string_view::npos && fraction.empty()) ||
        fraction.find('.') != std::string_view::npos) {
        throwNotADuration(text, "the number is malformed");
    }

    // The bound leaves room for one more unit, so adding the fraction below cannot overflow.
    Duration result = 0;
    for (const char digit : whole) {
        if (result > (kMaxDuration / scale - 1 - (digit - '0')) / 10) {
            throwNotADuration(text, "it is too long");
        }
        result = result * 10 + (digit - '0');
    }
    result *= scale;

    // Each fractional digit is worth a tenth of the previous one; below a nanosecond, only zeros
    // are allowed.
    Duration place = scale;
    for (const char digit : fraction) {
        if (place % 10 != 0) {
            if (digit != '0') {
                throwNotADuration(text, "it is finer than a nanosecond");
            }
            continue;
        }
        place /= 10;
        result += (digit - '0') * place;
    }
    return result;
}

void Parameters::set(std::string name, Value value) {
    values_.insert_or_assign(std::move(name), std::move(value));
}

bool Parameters::contains(std::string_view name) const { return values_.contains(name); }

bool Parameters::flag(std::string_view name, bool fallback) const {
    const Value* value = read(name);
    if (value == nullptr) {
        return fallback;
    }
    if (const auto* flag = std::get_if<bool>(value)) {
        return *flag;
    }
    throw std::invalid_argument(std::format("parameter '{}' must be true or false", name));
}

std::int64_t Parameters::integer(std::string_view name, std::int64_t fallback) const {
    const Value* value = read(name);
    if (value == nullptr) {
        return fallback;
    }
    if (const auto* integer = std::get_if<std::int64_t>(value)) {
        return *integer;
    }
    throw std::invalid_argument(std::format("parameter '{}' must be a whole number", name));
}

double Parameters::number(std::string_view name, double fallback) const {
    const Value* value = read(name);
    if (value == nullptr) {
        return fallback;
    }
    if (const auto* integer = std::get_if<std::int64_t>(value)) {
        return static_cast<double>(*integer);
    }
    if (const auto* decimal = std::get_if<double>(value)) {
        return *decimal;
    }
    throw std::invalid_argument(std::format("parameter '{}' must be a number", name));
}

std::string Parameters::text(std::string_view name, std::string_view fallback) const {
    const Value* value = read(name);
    if (value == nullptr) {
        return std::string{fallback};
    }
    if (const auto* text = std::get_if<std::string>(value)) {
        return *text;
    }
    throw std::invalid_argument(std::format("parameter '{}' must be text", name));
}

Duration Parameters::duration(std::string_view name, Duration fallback) const {
    const Value* value = read(name);
    if (value == nullptr) {
        return fallback;
    }
    const auto* text = std::get_if<std::string>(value);
    if (text == nullptr) {
        throw std::invalid_argument(
            std::format("parameter '{}' must be a duration such as \"1.5ms\"", name));
    }
    try {
        return parseDuration(*text);
    } catch (const std::invalid_argument& error) {
        throw std::invalid_argument(std::format("parameter '{}': {}", name, error.what()));
    }
}

std::vector<std::string> Parameters::unusedNames() const {
    std::vector<std::string> unused;
    for (const auto& entry : values_) {
        if (!read_.contains(entry.first)) {
            unused.push_back(entry.first);
        }
    }
    return unused;
}

const Parameters::Value* Parameters::read(std::string_view name) const {
    const auto entry = values_.find(name);
    if (entry == values_.end()) {
        return nullptr;
    }
    read_.emplace(name);
    return &entry->second;
}

} // namespace crowdbook

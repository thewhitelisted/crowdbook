#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "crowdbook/types.hpp"

namespace crowdbook {

// Parses a duration such as "250ns", "50us", "1.5ms" or "2s" into nanoseconds. The number must
// not be negative and must come to a whole number of nanoseconds. Throws std::invalid_argument
// otherwise.
[[nodiscard]] Duration parseDuration(std::string_view text);

// Named settings for an agent, read from a scenario file or set in code. Each getter returns the
// value stored under `name`, or `fallback` if there is none, and remembers that `name` was read,
// so names that were set but never read, usually typos, can be reported. A value of the wrong type
// throws std::invalid_argument naming the parameter.
class Parameters {
public:
    using Value = std::variant<bool, std::int64_t, double, std::string>;

    void set(std::string name, Value value);
    [[nodiscard]] bool contains(std::string_view name) const;

    [[nodiscard]] bool flag(std::string_view name, bool fallback) const;
    [[nodiscard]] std::int64_t integer(std::string_view name, std::int64_t fallback) const;
    // Accepts whole numbers as well as decimals.
    [[nodiscard]] double number(std::string_view name, double fallback) const;
    [[nodiscard]] std::string text(std::string_view name, std::string_view fallback) const;
    // A duration written as text, such as "1.5ms"; see parseDuration.
    [[nodiscard]] Duration duration(std::string_view name, Duration fallback) const;

    // Names that hold a value but were never read, in alphabetical order.
    [[nodiscard]] std::vector<std::string> unusedNames() const;

private:
    const Value* read(std::string_view name) const;

    std::map<std::string, Value, std::less<>> values_;
    mutable std::set<std::string, std::less<>> read_;
};

} // namespace crowdbook

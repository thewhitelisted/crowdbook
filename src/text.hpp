#pragma once

#include <charconv>
#include <concepts>
#include <string>
#include <string_view>

#include "crowdbook/types.hpp"

// Appending numbers to text without formatting machinery: the event log and the protocol write
// one line per event, so this is on the hot path of every logged or served run.
namespace crowdbook::text {

void appendInteger(std::string& out, std::integral auto value) {
    char digits[24];
    const auto [end, error] = std::to_chars(digits, digits + sizeof digits, value);
    out.append(digits, end);
}

// A fee in tick-lots, exactly, as formatFee writes it: "-1.25", "0.1", "3".
void appendFee(std::string& out, Fee fee);

} // namespace crowdbook::text

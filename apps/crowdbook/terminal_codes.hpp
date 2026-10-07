#pragma once

#include <optional>
#include <string>
#include <vector>

namespace crowdbook {

// A key the trading screen understands.
struct Key {
    enum class Kind { Character, Up, Down, PageUp, PageDown };
    Kind kind = Kind::Character;
    char character = 0; // for Kind::Character

    friend bool operator==(const Key&, const Key&) = default;
};

// Turns the first key in `bytes`, as a terminal sends them, into a Key and removes its bytes.
// Ctrl-C comes out as 'q'. Escape sequences for keys the screen does not use are skipped. Returns
// nullopt once `bytes` holds no key.
[[nodiscard]] std::optional<Key> takeKey(std::string& bytes);

// The bytes that replace the screen with these lines. They leave the cursor at the end of the last
// line rather than below it, because a newline on the terminal's bottom row would scroll the first
// line off the screen.
[[nodiscard]] std::string frame(const std::vector<std::string>& lines);

} // namespace crowdbook

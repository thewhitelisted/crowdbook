#include "terminal_codes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace crowdbook {

namespace {

// The length of the escape sequence at the start of `bytes`: escape, '[', any parameters and a
// final character for a control sequence; escape, 'O' and one character for a function key;
// escape and one character for an Alt key; or escape alone.
std::size_t escapeLength(std::string_view bytes) {
    if (bytes.size() < 2) {
        return bytes.size();
    }
    if (bytes[1] == 'O') {
        return std::min<std::size_t>(3, bytes.size());
    }
    if (bytes[1] != '[') {
        return 2;
    }
    std::size_t end = 2;
    while (end < bytes.size() && bytes[end] >= 0x20 && bytes[end] <= 0x3f) {
        ++end; // parameters, such as the "1;5" of Ctrl-Up
    }
    if (end < bytes.size() && bytes[end] >= 0x40 && bytes[end] <= 0x7e) {
        ++end; // the final character
    }
    return end;
}

} // namespace

std::optional<Key> takeKey(std::string& bytes) {
    // Sized by its entries, so that none can be left empty: an empty one would match anything.
    static constexpr auto kSequences = std::to_array<std::pair<std::string_view, Key::Kind>>({
        {"\x1b[A", Key::Kind::Up},
        {"\x1bOA", Key::Kind::Up},
        {"\x1b[B", Key::Kind::Down},
        {"\x1bOB", Key::Kind::Down},
        {"\x1b[5~", Key::Kind::PageUp},
        {"\x1b[6~", Key::Kind::PageDown},
    });
    while (!bytes.empty()) {
        for (const auto& [sequence, kind] : kSequences) {
            if (bytes.starts_with(sequence)) {
                bytes.erase(0, sequence.size());
                return Key{.kind = kind};
            }
        }
        if (bytes.front() == '\x1b') {
            bytes.erase(0, escapeLength(bytes));
            continue;
        }
        const char character = bytes.front();
        bytes.erase(0, 1);
        return Key{.character = character == '\x03' ? 'q' : character};
    }
    return std::nullopt;
}

std::string frame(const std::vector<std::string>& lines) {
    // Home; each line, clearing what is left of the old one; then clear everything below.
    std::string text = "\x1b[H";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            text += "\r\n";
        }
        text += lines[i];
        text += "\x1b[K";
    }
    text += "\x1b[J";
    return text;
}

} // namespace crowdbook

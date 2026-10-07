#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "terminal_codes.hpp"

namespace crowdbook {

// The terminal set up for a full-screen display: keys arrive one at a time without echo, and
// drawing happens on the alternate screen, so the shell's screen comes back untouched. The
// destructor restores everything. POSIX terminals only.
class RawTerminal {
public:
    // Throws std::runtime_error unless standard input and output are a terminal.
    RawTerminal();
    RawTerminal(const RawTerminal&) = delete;
    RawTerminal& operator=(const RawTerminal&) = delete;
    ~RawTerminal();

    // The next key pressed, waiting up to `timeout` if none is waiting, or nullopt. Keys that
    // arrive together are returned one per call. Ctrl-C arrives as 'q'.
    [[nodiscard]] std::optional<Key> readKey(std::chrono::milliseconds timeout);
    // Replaces the screen with these lines.
    void draw(const std::vector<std::string>& lines);
    // Rows and columns.
    [[nodiscard]] std::pair<std::size_t, std::size_t> size() const;

private:
    struct Saved;
    std::unique_ptr<Saved> saved_;
    std::string pending_; // bytes read but not yet turned into keys
};

} // namespace crowdbook

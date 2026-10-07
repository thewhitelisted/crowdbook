#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "terminal_codes.hpp"

namespace crowdbook {
namespace {

// Every key in `bytes`, in order.
std::vector<Key> keysIn(std::string bytes) {
    std::vector<Key> keys;
    while (const std::optional<Key> key = takeKey(bytes)) {
        keys.push_back(*key);
    }
    EXPECT_EQ(bytes, "");
    return keys;
}

Key typed(char character) { return Key{.character = character}; }
Key pressed(Key::Kind kind) { return Key{.kind = kind}; }

TEST(TerminalCodesTest, TakesKeysThatArriveTogetherOneAtATime) {
    EXPECT_EQ(keysIn("bb\x1b[A\x1b[5~s\x1b[B\x1b[6~"),
              (std::vector<Key>{typed('b'), typed('b'), pressed(Key::Kind::Up),
                                pressed(Key::Kind::PageUp), typed('s'), pressed(Key::Kind::Down),
                                pressed(Key::Kind::PageDown)}));
}

TEST(TerminalCodesTest, ReadsCtrlCAsQuitAndBothFormsOfTheArrows) {
    EXPECT_EQ(keysIn("\x03"), std::vector<Key>{typed('q')});
    EXPECT_EQ(keysIn("\x1bOA\x1bOB"),
              (std::vector<Key>{pressed(Key::Kind::Up), pressed(Key::Kind::Down)}));
}

TEST(TerminalCodesTest, SkipsTheWholeOfOtherKeysSequences) {
    // Ctrl-Up, F1, Alt-b, Delete and a lone escape: none of their characters are keys.
    EXPECT_EQ(keysIn("\x1b[1;5Ab\x1bOPs\x1b"
                     "bc\x1b[3~m\x1b"),
              (std::vector<Key>{typed('b'), typed('s'), typed('c'), typed('m')}));
    EXPECT_EQ(keysIn(""), std::vector<Key>{});
}

TEST(TerminalCodesTest, AFrameEndsOnItsLastLine) {
    // A newline after the last line would scroll the screen when the lines fill it.
    EXPECT_EQ(frame({"one", "two"}), "\x1b[Hone\x1b[K\r\ntwo\x1b[K\x1b[J");
    EXPECT_EQ(frame({}), "\x1b[H\x1b[J");
}

} // namespace
} // namespace crowdbook

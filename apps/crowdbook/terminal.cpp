#include "terminal.hpp"

#include <array>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace crowdbook {

namespace {

void writeAll(std::string_view text) {
    while (!text.empty()) {
        const ssize_t written = ::write(STDOUT_FILENO, text.data(), text.size());
        if (written <= 0) {
            return;
        }
        text.remove_prefix(static_cast<std::size_t>(written));
    }
}

// Reads whatever bytes are waiting, up to the buffer's size.
std::string readWaiting() {
    std::array<char, 256> buffer{};
    const ssize_t count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
    return count > 0 ? std::string(buffer.data(), static_cast<std::size_t>(count)) : std::string{};
}

} // namespace

struct RawTerminal::Saved {
    termios original{};
};

RawTerminal::RawTerminal() : saved_(std::make_unique<Saved>()) {
    if (::isatty(STDIN_FILENO) == 0 || ::isatty(STDOUT_FILENO) == 0) {
        throw std::runtime_error("this needs a terminal");
    }
    ::tcgetattr(STDIN_FILENO, &saved_->original);
    termios raw = saved_->original;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    writeAll("\x1b[?1049h\x1b[?25l"); // the alternate screen, without a cursor
}

RawTerminal::~RawTerminal() {
    writeAll("\x1b[?25h\x1b[?1049l");
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_->original);
}

std::optional<Key> RawTerminal::readKey(std::chrono::milliseconds timeout) {
    if (pending_.empty()) {
        pollfd input{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
        if (::poll(&input, 1, static_cast<int>(timeout.count())) <= 0) {
            return std::nullopt;
        }
        pending_ += readWaiting();
    }
    return takeKey(pending_);
}

void RawTerminal::draw(const std::vector<std::string>& lines) { writeAll(frame(lines)); }

std::pair<std::size_t, std::size_t> RawTerminal::size() const {
    winsize window{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) != 0 || window.ws_row == 0 ||
        window.ws_col == 0) {
        return {24, 80};
    }
    return {window.ws_row, window.ws_col};
}

} // namespace crowdbook

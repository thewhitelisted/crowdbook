#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "crowdbook/ledger.hpp"
#include "crowdbook/messages.hpp"
#include "crowdbook/scoring.hpp"
#include "crowdbook/simulation.hpp"
#include "crowdbook/types.hpp"

// The messages a client of a served market exchanges with it, and their encoding as JSON lines.
// docs/protocol.md specifies both.
namespace crowdbook::protocol {

inline constexpr std::int64_t kVersion = 1;
// The longest message a client may send, including its newline.
inline constexpr std::size_t kMaxLineLength = 4'096;
inline constexpr std::size_t kMaxSeatLength = 32;
inline constexpr std::size_t kMaxTokenLength = 128;

// A malformed message. The text says what is wrong with it.
class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A seat name: 1 to kMaxSeatLength letters, digits, '-' or '_'.
[[nodiscard]] bool validSeat(std::string_view seat) noexcept;

// Client messages. Orders are named by the client's own ids, carried in clientOrderId.

struct Hello {
    std::int64_t protocol = kVersion;
    std::string seat{};
    std::string token{}; // empty when not given

    friend bool operator==(const Hello&, const Hello&) = default;
};

using ClientMessage = std::variant<Hello, NewOrder, CancelOrder, ModifyOrder>;

// Server messages.

struct AccountState {
    Cash initialCash = 0;
    Quantity initialPosition = 0;
    Cash cash = 0;
    Quantity position = 0;
    Fee fees = 0;
    Quantity maxPosition = 0;
    Quantity maxOrderQuantity = 0;

    friend bool operator==(const AccountState&, const AccountState&) = default;
};

// The challenge a scenario offers, if it is one.
struct ChallengeInfo {
    std::string name{};
    std::string briefing{};

    friend bool operator==(const ChallengeInfo&, const ChallengeInfo&) = default;
};

struct Welcome {
    std::int64_t protocol = kVersion;
    std::string seat{};
    bool started = false;
    Timestamp time = 0;
    Duration duration = 0;
    Price referencePrice = 0;
    Price maxPrice = kMaxPrice; // the highest limit price: 99 in a prediction market
    std::size_t depthLevels = 0;
    Fee makerFee = 0;
    Fee takerFee = 0;
    Fee auctionFee = 0;
    Phase phase = Phase::Continuous; // the market's phase as the seat sees it now
    Latency latency{};
    AccountState account{};
    std::vector<OwnOrder> orders{}; // the seat's live orders, by the client's ids
    std::optional<ChallengeInfo> challenge{};
    std::optional<ScoringConfig> scoring{}; // how the seat is scored, if it is

    friend bool operator==(const Welcome&, const Welcome&) = default;
};

struct Start {
    Timestamp time = 0;

    friend bool operator==(const Start&, const Start&) = default;
};

struct Clock {
    Timestamp time = 0;

    friend bool operator==(const Clock&, const Clock&) = default;
};

// An order event or market data, as it reached the seat at `time`. Order events name orders by the
// client's ids, and their agent field is not sent: decoding leaves it 0.
struct MarketMessage {
    Timestamp time = 0;
    Event event{};

    friend bool operator==(const MarketMessage&, const MarketMessage&) = default;
};

struct End {
    Timestamp time = 0;
    Cash cash = 0;
    Quantity position = 0;
    Fee fees = 0;
    Cash pnl = 0;
    std::optional<Score> score{}; // when the scenario has scoring

    friend bool operator==(const End&, const End&) = default;
};

struct Error {
    std::string message{};
    bool fatal = false;

    friend bool operator==(const Error&, const Error&) = default;
};

using ServerMessage = std::variant<Welcome, Start, Clock, MarketMessage, End, Error>;

// Each encodes one message as a line of JSON, ending in '\n'.
[[nodiscard]] std::string encode(const ClientMessage& message);
[[nodiscard]] std::string encode(const ServerMessage& message);
// The same, appended to `out`, so that a server can write straight into a connection's buffer.
void encodeTo(std::string& out, const ClientMessage& message);
void encodeTo(std::string& out, const ServerMessage& message);

// Each decodes one line, with or without its '\n'. Throws ProtocolError for anything that is not
// a well-formed message of its direction, including unknown or repeated fields, values of the
// wrong type, a seat name that validSeat rejects and a token that is too long or not printable.
[[nodiscard]] ClientMessage decodeClient(std::string_view line);
[[nodiscard]] ServerMessage decodeServer(std::string_view line);

// Spellings on the wire. A reject reason is written in lowercase with hyphens:
// "position-limit".
[[nodiscard]] std::string wireName(RejectReason reason);

} // namespace crowdbook::protocol

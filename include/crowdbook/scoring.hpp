#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "crowdbook/event_log.hpp"
#include "crowdbook/exchange.hpp"
#include "crowdbook/messages.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// Scores are in points: thousandths of a tick-lot, the unit fees use.
using Points = std::int64_t;
inline constexpr Points kPointsPerTickLot = kFeeUnitsPerTickLot;

// What positions are valued at when a session is scored.
enum class Mark : std::uint8_t {
    LastTrade, // the last trade price
    Value,     // the true value at the end; the scenario needs a fundamental value
};

// The price a target's paper portfolio trades at.
enum class Benchmark : std::uint8_t {
    Vwap,      // the volume-weighted average price of every trade in the session
    Reference, // the scenario's reference price
};

// A large order a seat is asked to work.
struct Target {
    Side side = Side::Buy;
    Quantity quantity = 0;
    Benchmark benchmark = Benchmark::Vwap;
    Points unfinishedPenalty = 0; // per lot of the target not done

    friend bool operator==(const Target&, const Target&) = default;
};

struct ScoringConfig {
    Mark mark = Mark::LastTrade;
    Points inventoryPenalty = 0; // per lot per second held
    Points closePenalty = 0;     // per lot held at the end
    Cash maxLoss = 0;            // in tick-lots; 0 for no loss limit
    std::optional<Target> target{};

    friend bool operator==(const ScoringConfig&, const ScoringConfig&) = default;
};

// One seat's score and its parts, in points:
// total = pnl − inventory − close − paper − unfinished.
struct Score {
    Points pnl = 0;        // change in cash and position valued at the mark, net of fees
    Points inventory = 0;  // the inventory penalty times the lot-seconds held
    Points close = 0;      // the close penalty times the lots held at the end
    Points paper = 0;      // the target's paper portfolio's PnL; 0 without a target
    Points unfinished = 0; // the unfinished penalty times the target's lots not done
    Points total = 0;
    Quantity unfinishedLots = 0;
    std::optional<Timestamp> stoppedAt{}; // when the loss limit stopped the seat, if it did

    friend bool operator==(const Score&, const Score&) = default;
};

// Watches the exchange's activity and scores some of its agents, the seats. Add it to the
// simulation's sinks before the run starts.
class Scorer final : public EventSink {
public:
    // Throws std::invalid_argument for a target without a positive quantity or penalties that
    // are negative.
    Scorer(ScoringConfig config, Price referencePrice);

    // Scores this agent, which starts with these balances.
    void addSeat(AgentId agent, Cash initialCash, Quantity initialPosition);

    void onRequest(Timestamp time, AgentId agent, const Request& request) override;
    void onEvent(Timestamp time, const Event& event) override;

    // When the agent's loss reached the limit, if it has. Throws std::out_of_range for an agent
    // that is not a seat.
    [[nodiscard]] std::optional<Timestamp> stoppedAt(AgentId agent) const;
    // The agent's score if the session ended at `end`, with `value` the true value then, which
    // Mark::Value needs. Throws std::out_of_range for an agent that is not a seat, and
    // std::invalid_argument for Mark::Value without a value.
    [[nodiscard]] Score score(AgentId agent, Timestamp end, std::optional<double> value) const;
    [[nodiscard]] const ScoringConfig& config() const noexcept { return config_; }

private:
    struct Seat {
        Cash initialCash = 0;
        Quantity initialPosition = 0;
        Cash cash = 0;
        Quantity position = 0;
        Fee fees = 0;
        double lotNanoseconds = 0.0; // |position| integrated over time, up to `since`
        Timestamp since = 0;
        std::optional<Timestamp> stoppedAt{};
    };

    ScoringConfig config_;
    Price referencePrice_;
    std::unordered_map<AgentId, Seat> seats_;
    std::vector<AgentId> order_; // seats in the order added, for checks in a fixed order
    std::optional<Price> lastPrice_;
    Cash tradedValue_ = 0; // price times quantity, summed over every trade
    Quantity tradedVolume_ = 0;
};

[[nodiscard]] std::string_view toString(Mark mark) noexcept;
[[nodiscard]] std::string_view toString(Benchmark benchmark) noexcept;

} // namespace crowdbook

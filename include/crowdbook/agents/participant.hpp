#pragma once

#include <cstddef>
#include <deque>
#include <optional>

#include "crowdbook/agent.hpp"
#include "crowdbook/messages.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// A trade as a participant saw it, with the time it arrived.
struct TapeEntry {
    Timestamp time = 0;
    Trade trade{};

    friend bool operator==(const TapeEntry&, const TapeEntry&) = default;
};

// The agent a person, or a program outside the simulation, trades through. It makes no decisions
// of its own: its requests come from Simulation::act. It streams market data and keeps the latest
// trades and the latest rejection, for display.
class Participant final : public Agent {
public:
    explicit Participant(std::size_t tapeLength = 30);

    void onTrade(AgentContext& context, const Trade& trade) override;
    void onRejected(AgentContext& context, const OrderRejected& event) override;

    // The latest trades, newest first, at most tapeLength of them.
    [[nodiscard]] const std::deque<TapeEntry>& tape() const noexcept { return tape_; }
    [[nodiscard]] const std::optional<OrderRejected>& lastRejection() const noexcept {
        return lastRejection_;
    }

private:
    std::size_t tapeLength_;
    std::deque<TapeEntry> tape_;
    std::optional<OrderRejected> lastRejection_;
};

} // namespace crowdbook

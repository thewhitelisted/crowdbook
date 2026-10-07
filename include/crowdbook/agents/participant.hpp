#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <utility>

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
// trades and the latest rejection, for display, and can pass every event it receives on to a
// listener, which is how a gateway relays them to a client.
class Participant final : public Agent {
public:
    // Called with every event the participant receives and the time it arrived, after the
    // event has been applied to the participant's ledger.
    using Listener = std::function<void(AgentContext& context, const Event& event)>;

    explicit Participant(std::size_t tapeLength = 30);

    // Replaces the listener; an empty one stops relaying.
    void setListener(Listener listener) { listener_ = std::move(listener); }

    void onAccepted(AgentContext& context, const OrderAccepted& event) override;
    void onRejected(AgentContext& context, const OrderRejected& event) override;
    void onModified(AgentContext& context, const OrderModified& event) override;
    void onFilled(AgentContext& context, const OrderFilled& event) override;
    void onCancelled(AgentContext& context, const OrderCancelled& event) override;
    void onTrade(AgentContext& context, const Trade& trade) override;
    void onTopOfBook(AgentContext& context, const TopOfBook& top) override;
    void onDepth(AgentContext& context, const BookDepth& depth) override;

    // The latest trades, newest first, at most tapeLength of them.
    [[nodiscard]] const std::deque<TapeEntry>& tape() const noexcept { return tape_; }
    [[nodiscard]] const std::optional<OrderRejected>& lastRejection() const noexcept {
        return lastRejection_;
    }

private:
    void relay(AgentContext& context, const Event& event) const {
        if (listener_) {
            listener_(context, event);
        }
    }

    std::size_t tapeLength_;
    std::deque<TapeEntry> tape_;
    std::optional<OrderRejected> lastRejection_;
    Listener listener_;
};

} // namespace crowdbook

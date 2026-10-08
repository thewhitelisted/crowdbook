#pragma once

#include <functional>
#include <optional>
#include <utility>

#include "crowdbook/agent.hpp"
#include "crowdbook/messages.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// The agent a person, or a program outside the simulation, trades through. It makes no decisions
// of its own: its requests come from Simulation::act. It streams market data, keeps the latest
// rejection, and can pass every event it receives on to a listener, which is how a gateway relays
// them to a client.
class Participant final : public Agent {
public:
    // Called with every event the participant receives and the time it arrived, after the
    // event has been applied to the participant's ledger.
    using Listener = std::function<void(AgentContext& context, const Event& event)>;

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
    void onPhase(AgentContext& context, const PhaseChanged& phase) override;
    void onIndicative(AgentContext& context, const Indicative& indicative) override;

    [[nodiscard]] const std::optional<OrderRejected>& lastRejection() const noexcept {
        return lastRejection_;
    }

private:
    void relay(AgentContext& context, const Event& event) const {
        if (listener_) {
            listener_(context, event);
        }
    }

    std::optional<OrderRejected> lastRejection_;
    Listener listener_;
};

} // namespace crowdbook

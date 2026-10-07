#include "crowdbook/agents/participant.hpp"

namespace crowdbook {

Participant::Participant(std::size_t tapeLength) : tapeLength_(tapeLength) {}

void Participant::onTrade(AgentContext& context, const Trade& trade) {
    tape_.push_front({.time = context.now(), .trade = trade});
    if (tape_.size() > tapeLength_) {
        tape_.pop_back();
    }
}

void Participant::onRejected(AgentContext& /*context*/, const OrderRejected& event) {
    lastRejection_ = event;
}

} // namespace crowdbook

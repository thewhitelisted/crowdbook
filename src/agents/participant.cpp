#include "crowdbook/agents/participant.hpp"

namespace crowdbook {

Participant::Participant(std::size_t tapeLength) : tapeLength_(tapeLength) {}

void Participant::onAccepted(AgentContext& context, const OrderAccepted& event) {
    relay(context, event);
}

void Participant::onRejected(AgentContext& context, const OrderRejected& event) {
    lastRejection_ = event;
    relay(context, event);
}

void Participant::onModified(AgentContext& context, const OrderModified& event) {
    relay(context, event);
}

void Participant::onFilled(AgentContext& context, const OrderFilled& event) {
    relay(context, event);
}

void Participant::onCancelled(AgentContext& context, const OrderCancelled& event) {
    relay(context, event);
}

void Participant::onTrade(AgentContext& context, const Trade& trade) {
    tape_.push_front({.time = context.now(), .trade = trade});
    if (tape_.size() > tapeLength_) {
        tape_.pop_back();
    }
    relay(context, trade);
}

void Participant::onTopOfBook(AgentContext& context, const TopOfBook& top) {
    relay(context, top);
}

void Participant::onDepth(AgentContext& context, const BookDepth& depth) {
    relay(context, depth);
}

} // namespace crowdbook

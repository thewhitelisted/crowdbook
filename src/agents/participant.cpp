#include "crowdbook/agents/participant.hpp"

namespace crowdbook {

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
    relay(context, trade);
}

void Participant::onTopOfBook(AgentContext& context, const TopOfBook& top) {
    relay(context, top);
}

void Participant::onDepth(AgentContext& context, const BookDepth& depth) {
    relay(context, depth);
}

void Participant::onPhase(AgentContext& context, const PhaseChanged& phase) {
    relay(context, phase);
}

void Participant::onIndicative(AgentContext& context, const Indicative& indicative) {
    relay(context, indicative);
}

} // namespace crowdbook

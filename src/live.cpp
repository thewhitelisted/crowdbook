#include "crowdbook/live.hpp"

#include <cmath>
#include <format>
#include <stdexcept>
#include <variant>

namespace crowdbook {

namespace {

void checkSpeed(double speed) {
    if (!(speed > 0.0) || !std::isfinite(speed)) {
        throw std::invalid_argument("the speed must be positive and finite");
    }
}

} // namespace

Pacer::Pacer(std::int64_t wallNow, Timestamp simulatedNow, double speed)
    : wallAnchor_(wallNow), simulatedAnchor_(simulatedNow), speed_(speed) {
    checkSpeed(speed);
}

Timestamp Pacer::simulatedAt(std::int64_t wall) const noexcept {
    if (paused_ || wall <= wallAnchor_) {
        return simulatedAnchor_;
    }
    return simulatedAnchor_ + std::llround(static_cast<double>(wall - wallAnchor_) * speed_);
}

void Pacer::setSpeed(double speed, std::int64_t wall) {
    checkSpeed(speed);
    simulatedAnchor_ = simulatedAt(wall);
    wallAnchor_ = wall;
    speed_ = speed;
}

void Pacer::setPaused(bool paused, std::int64_t wall) {
    simulatedAnchor_ = simulatedAt(wall);
    wallAnchor_ = wall;
    paused_ = paused;
}

ClientOrderId perform(Simulation& simulation, AgentId agent, const SessionAction& action) {
    if (simulation.now() != action.time) {
        throw std::logic_error(std::format("an action at {} ns performed at {} ns", action.time,
                                           simulation.now()));
    }
    ClientOrderId id = 0;
    simulation.act(agent, [&](AgentContext& context) {
        if (const auto* order = std::get_if<NewOrder>(&action.request)) {
            id = context.submit(*order);
            if (order->clientOrderId != 0 && order->clientOrderId != id) {
                throw std::logic_error(std::format(
                    "a new order at {} ns got client order id {}, not the recorded {}",
                    action.time, id, order->clientOrderId));
            }
        } else if (const auto* cancel = std::get_if<CancelOrder>(&action.request)) {
            id = cancel->clientOrderId;
            context.cancel(id);
        } else {
            const auto& modify = std::get<ModifyOrder>(action.request);
            id = modify.clientOrderId;
            context.modify(id, modify.price, modify.quantity);
        }
    });
    return id;
}

} // namespace crowdbook

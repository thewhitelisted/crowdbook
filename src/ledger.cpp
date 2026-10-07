#include "crowdbook/ledger.hpp"

#include <format>
#include <stdexcept>

namespace crowdbook {

Ledger::Ledger(Cash cash, Quantity position) noexcept : cash_(cash), position_(position) {}

void Ledger::recordRequest(const Request& request) {
    if (const auto* newOrder = std::get_if<NewOrder>(&request)) {
        const bool inserted = orders_
                                  .try_emplace(newOrder->clientOrderId,
                                               OwnOrder{.clientOrderId = newOrder->clientOrderId,
                                                        .side = newOrder->side,
                                                        .type = newOrder->type,
                                                        .price = newOrder->price,
                                                        .leaves = newOrder->quantity})
                                  .second;
        if (!inserted) {
            throw std::logic_error(
                std::format("client order id {} is already in use", newOrder->clientOrderId));
        }
    } else if (const auto* cancel = std::get_if<CancelOrder>(&request)) {
        const auto open = orders_.find(cancel->clientOrderId);
        if (open != orders_.end()) {
            open->second.cancelRequested = true;
        }
    }
    // A modify changes nothing until the exchange confirms it.
}

void Ledger::apply(const Event& event) {
    if (const auto* accepted = std::get_if<OrderAccepted>(&event)) {
        OwnOrder& order = known(accepted->clientOrderId);
        order.acknowledged = true;
        order.orderId = accepted->orderId;
    } else if (const auto* rejected = std::get_if<OrderRejected>(&event)) {
        if (rejected->request == RequestKind::New) {
            forget(rejected->clientOrderId);
        } else if (rejected->request == RequestKind::Cancel) {
            // The cancel failed. An order that is still open may yet fill; one that is gone
            // finished while the cancel was on its way.
            const auto order = orders_.find(rejected->clientOrderId);
            if (order != orders_.end()) {
                order->second.cancelRequested = false;
            }
        }
        // A rejected modify leaves the order as it was.
    } else if (const auto* modified = std::get_if<OrderModified>(&event)) {
        OwnOrder& order = known(modified->clientOrderId);
        order.price = modified->price;
        order.leaves = modified->quantity;
    } else if (const auto* filled = std::get_if<OrderFilled>(&event)) {
        OwnOrder& order = known(filled->clientOrderId);
        const Cash notional = filled->price * filled->quantity;
        if (filled->side == Side::Buy) {
            position_ += filled->quantity;
            cash_ -= notional;
        } else {
            position_ -= filled->quantity;
            cash_ += notional;
        }
        fees_ += filled->fee;
        order.leaves = filled->leavesQuantity;
        if (order.leaves == 0) {
            orders_.erase(filled->clientOrderId);
        }
    } else if (const auto* cancelled = std::get_if<OrderCancelled>(&event)) {
        forget(cancelled->clientOrderId);
    }
}

const OwnOrder* Ledger::find(ClientOrderId clientOrderId) const {
    const auto order = orders_.find(clientOrderId);
    return order == orders_.end() ? nullptr : &order->second;
}

Quantity Ledger::openQuantity(Side side) const noexcept {
    Quantity total = 0;
    for (const auto& entry : orders_) {
        if (entry.second.side == side) {
            total += entry.second.leaves;
        }
    }
    return total;
}

OwnOrder& Ledger::known(ClientOrderId clientOrderId) {
    const auto order = orders_.find(clientOrderId);
    if (order == orders_.end()) {
        throw std::logic_error(std::format(
            "event for client order id {}, which is not open or in flight", clientOrderId));
    }
    return order->second;
}

void Ledger::forget(ClientOrderId clientOrderId) {
    if (orders_.erase(clientOrderId) == 0) {
        throw std::logic_error(std::format(
            "event for client order id {}, which is not open or in flight", clientOrderId));
    }
}

} // namespace crowdbook

#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "crowdbook/agent.hpp"
#include "crowdbook/ledger.hpp"
#include "crowdbook/messages.hpp"
#include "crowdbook/random.hpp"

namespace crowdbook::test {

// An AgentContext for testing one agent on its own. It records everything the agent sends and
// lets the test play the exchange by feeding events to the agent's ledger, as the simulation does
// before each callback.
class FakeContext final : public AgentContext {
public:
    explicit FakeContext(std::uint64_t seed = 1, AgentId id = 1)
        : id_(id), random_(seed, 2 * std::uint64_t{id}) {}

    [[nodiscard]] AgentId id() const noexcept override { return id_; }
    [[nodiscard]] Timestamp now() const noexcept override { return now_; }
    [[nodiscard]] Random& random() noexcept override { return random_; }
    [[nodiscard]] const Ledger& ledger() const noexcept override { return ledger_; }
    [[nodiscard]] MarketSnapshot market() const override { return snapshot; }

    ClientOrderId submit(NewOrder order) override {
        order.clientOrderId = nextClientOrderId_++;
        record(order);
        return order.clientOrderId;
    }

    void cancel(ClientOrderId clientOrderId) override {
        record(CancelOrder{.clientOrderId = clientOrderId});
    }

    void modify(ClientOrderId clientOrderId, Price price, Quantity quantity) override {
        record(ModifyOrder{.clientOrderId = clientOrderId, .price = price, .quantity = quantity});
    }

    void wakeAt(Timestamp time, std::uint64_t tag) override {
        wakeups.emplace_back(std::max(time, now_), tag);
    }

    void setNow(Timestamp time) noexcept { now_ = time; }

    // Requests sent since the last call.
    std::vector<Request> takeSent() { return std::exchange(sent, {}); }

    // Plays the exchange: acknowledges an order the agent sent.
    void accept(ClientOrderId clientOrderId) {
        const OwnOrder& order = *ledger_.find(clientOrderId);
        ledger_.apply(OrderAccepted{.agent = id_,
                                    .clientOrderId = clientOrderId,
                                    .orderId = clientOrderId,
                                    .side = order.side,
                                    .type = order.type,
                                    .price = order.price,
                                    .quantity = order.leaves});
    }

    // Plays the exchange: fills part or all of an acknowledged order at its price.
    void fill(ClientOrderId clientOrderId, Quantity quantity) {
        const OwnOrder& order = *ledger_.find(clientOrderId);
        ledger_.apply(OrderFilled{.agent = id_,
                                  .clientOrderId = clientOrderId,
                                  .orderId = order.orderId,
                                  .side = order.side,
                                  .price = order.price,
                                  .quantity = quantity,
                                  .leavesQuantity = order.leaves - quantity});
    }

    // Plays the exchange: cancels what is left of an order, as it does with the part of a market
    // order the book cannot fill. Returns the event, for the test to hand to the agent.
    OrderCancelled cancelRest(ClientOrderId clientOrderId) {
        const OwnOrder& order = *ledger_.find(clientOrderId);
        const OrderCancelled event{.agent = id_,
                                   .clientOrderId = clientOrderId,
                                   .orderId = order.orderId,
                                   .quantity = order.leaves,
                                   .reason = CancelReason::ImmediateOrCancel};
        ledger_.apply(event);
        return event;
    }

    // Plays the exchange: rejects a new order. Returns the event, for the test to hand to the
    // agent.
    OrderRejected reject(ClientOrderId clientOrderId, RejectReason reason) {
        const OrderRejected event{.agent = id_,
                                  .clientOrderId = clientOrderId,
                                  .request = RequestKind::New,
                                  .reason = reason};
        ledger_.apply(event);
        return event;
    }

    // Plays the exchange: confirms a modify.
    void confirmModify(ClientOrderId clientOrderId, Price price, Quantity quantity) {
        ledger_.apply(OrderModified{.agent = id_,
                                    .clientOrderId = clientOrderId,
                                    .orderId = ledger_.find(clientOrderId)->orderId,
                                    .price = price,
                                    .quantity = quantity});
    }

    MarketSnapshot snapshot; // what market() returns; set it to show the agent a market
    std::vector<Request> sent;
    std::vector<std::pair<Timestamp, std::uint64_t>> wakeups;

private:
    void record(const Request& request) {
        ledger_.recordRequest(request);
        sent.push_back(request);
    }

    AgentId id_;
    Timestamp now_ = 0;
    Random random_;
    Ledger ledger_;
    ClientOrderId nextClientOrderId_ = 1;
};

// Starts a fresh agent from `make` under each seed from 1 to `seeds` and returns the time each
// first asked to wake up.
template <typename MakeAgent>
std::vector<Timestamp> firstWakeups(const MakeAgent& make, std::uint64_t seeds) {
    std::vector<Timestamp> times;
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
        const auto agent = make();
        FakeContext context{seed};
        agent->onStart(context);
        times.push_back(context.wakeups.at(0).first);
    }
    return times;
}

} // namespace crowdbook::test

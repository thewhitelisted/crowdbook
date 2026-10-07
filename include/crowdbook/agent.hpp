#pragma once

#include <cstdint>

#include "crowdbook/ledger.hpp"
#include "crowdbook/messages.hpp"
#include "crowdbook/random.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// What an agent can see and do from inside a callback. Nothing reaches the exchange instantly:
// each request arrives after the agent's latency, and the replies take time to come back.
class AgentContext {
public:
    AgentContext() = default;
    AgentContext(const AgentContext&) = delete;
    AgentContext& operator=(const AgentContext&) = delete;
    AgentContext(AgentContext&&) = delete;
    AgentContext& operator=(AgentContext&&) = delete;
    virtual ~AgentContext() = default;

    [[nodiscard]] virtual AgentId id() const noexcept = 0;
    [[nodiscard]] virtual Timestamp now() const noexcept = 0;
    // The agent's own random stream, determined by the run's seed and the agent's id.
    [[nodiscard]] virtual Random& random() noexcept = 0;
    // The agent's cash, position and orders as it knows them from its own requests and events.
    [[nodiscard]] virtual const Ledger& ledger() const noexcept = 0;

    // Sends a new order and returns the client order id that names it in later events. The
    // order's own clientOrderId is ignored; the context assigns a fresh one.
    virtual ClientOrderId submit(NewOrder order) = 0;
    virtual void cancel(ClientOrderId clientOrderId) = 0;
    virtual void modify(ClientOrderId clientOrderId, Price price, Quantity quantity) = 0;
    // Asks for onWakeup at `time`, or as soon as possible if that time has passed. `tag` is
    // passed back so an agent can tell its timers apart.
    virtual void wakeAt(Timestamp time, std::uint64_t tag) = 0;

    ClientOrderId submitLimit(Side side, Price price, Quantity quantity,
                              TimeInForce timeInForce = TimeInForce::GoodTillCancel) {
        return submit({.side = side,
                       .type = OrderType::Limit,
                       .timeInForce = timeInForce,
                       .price = price,
                       .quantity = quantity});
    }

    ClientOrderId submitMarket(Side side, Quantity quantity) {
        return submit({.side = side, .type = OrderType::Market, .quantity = quantity});
    }

    void wakeAfter(Duration delay, std::uint64_t tag = 0) { wakeAt(now() + delay, tag); }
};

// Base class for trading agents: override the callbacks you need. By the time a callback for one
// of the agent's own events runs, that event has already been applied to the context's ledger.
class Agent {
public:
    Agent() = default;
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;
    Agent(Agent&&) = delete;
    Agent& operator=(Agent&&) = delete;
    virtual ~Agent() = default;

    virtual void onStart(AgentContext& /*context*/) {}
    virtual void onWakeup(AgentContext& /*context*/, std::uint64_t /*tag*/) {}
    virtual void onAccepted(AgentContext& /*context*/, const OrderAccepted& /*event*/) {}
    virtual void onRejected(AgentContext& /*context*/, const OrderRejected& /*event*/) {}
    virtual void onModified(AgentContext& /*context*/, const OrderModified& /*event*/) {}
    virtual void onFilled(AgentContext& /*context*/, const OrderFilled& /*event*/) {}
    virtual void onCancelled(AgentContext& /*context*/, const OrderCancelled& /*event*/) {}
    virtual void onTrade(AgentContext& /*context*/, const Trade& /*trade*/) {}
    virtual void onTopOfBook(AgentContext& /*context*/, const TopOfBook& /*top*/) {}
};

} // namespace crowdbook

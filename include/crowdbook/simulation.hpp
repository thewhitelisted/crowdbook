#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "crowdbook/agent.hpp"
#include "crowdbook/event_log.hpp"
#include "crowdbook/exchange.hpp"
#include "crowdbook/ledger.hpp"
#include "crowdbook/messages.hpp"
#include "crowdbook/random.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// One-way network delays between an agent and the exchange.
struct Latency {
    Duration toExchange = 0;   // for the agent's requests
    Duration fromExchange = 0; // for every event sent to the agent, private or public
    Duration jitter = 0;       // extra delay per message, uniform in [0, jitter]
};

struct AgentOptions {
    AccountConfig account{};
    Latency latency{};
    std::optional<Timestamp> startTime{}; // when onStart runs; the current time if unset
};

// The discrete-event kernel. It owns the exchange and the agents and processes every message and
// wakeup in time order; items due at the same time run in the order they were scheduled. Each
// agent draws from its own random streams, so a run is fully determined by its seed. Messages on
// one link (an agent to the exchange, or the exchange to an agent) never overtake each other, even
// with jitter. The exchange itself takes no time to process a request.
class Simulation {
public:
    explicit Simulation(std::uint64_t seed);

    // Opens an account for the agent, schedules its start and returns its id: 1 for the first
    // agent, then 2, and so on. Throws std::invalid_argument if the agent is null, a latency is
    // negative, the start time has passed or the account limits are invalid.
    AgentId addAgent(std::unique_ptr<Agent> agent, const AgentOptions& options = {});

    // Reports every request reaching the exchange and every event it produces to `sink`, which is
    // not owned. Pass nullptr to stop.
    void setEventSink(EventSink* sink) noexcept { sink_ = sink; }

    // Processes everything scheduled up to and including `endTime`, then moves the clock there.
    void runUntil(Timestamp endTime);

    [[nodiscard]] Timestamp now() const noexcept { return now_; }
    // Messages and wakeups scheduled but not yet processed.
    [[nodiscard]] std::size_t pendingCount() const noexcept { return queue_.size(); }
    [[nodiscard]] const Exchange& exchange() const noexcept { return exchange_; }
    // Both throw std::out_of_range for an unknown id.
    [[nodiscard]] Agent& agent(AgentId id);
    [[nodiscard]] const Ledger& ledger(AgentId id) const;

private:
    class Context;

    struct Slot {
        std::unique_ptr<Agent> agent{};
        Latency latency{};
        Random random{0, 0};  // the agent's own stream
        Random network{0, 0}; // draws the jitter on the agent's links
        Ledger ledger{};
        ClientOrderId nextClientOrderId = 1;
        Timestamp lastArrivalAtExchange = 0; // links are first in, first out
        Timestamp lastArrivalAtAgent = 0;
    };

    struct Start {
        AgentId agent = 0;
    };
    struct Wakeup {
        AgentId agent = 0;
        std::uint64_t tag = 0;
    };
    // A request reaching the exchange.
    struct Arrival {
        AgentId sender = 0;
        Request request{};
    };
    // An event reaching an agent.
    struct Delivery {
        AgentId recipient = 0;
        Event event{};
    };
    using Action = std::variant<Start, Wakeup, Arrival, Delivery>;

    struct Scheduled {
        Timestamp time = 0;
        std::uint64_t sequence = 0;
        Action action{};
    };

    void schedule(Timestamp time, Action action);
    void send(AgentId sender, Request request);
    void deliver(AgentId recipient, const Event& event);
    Duration drawJitter(Slot& endpoint);
    Slot& slot(AgentId id);
    void process(const Start& start);
    void process(const Wakeup& wakeup);
    void process(const Arrival& arrival);
    void process(const Delivery& delivery);

    std::uint64_t seed_;
    Exchange exchange_;
    std::vector<Slot> slots_;
    std::vector<Scheduled> queue_; // a binary heap with the earliest item at the front
    std::uint64_t nextSequence_ = 0;
    Timestamp now_ = 0;
    EventSink* sink_ = nullptr;
    std::vector<Event> events_; // reused for every request
};

} // namespace crowdbook

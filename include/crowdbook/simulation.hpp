#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "crowdbook/agent.hpp"
#include "crowdbook/detail/ring.hpp"
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

    friend bool operator==(const Latency&, const Latency&) = default;
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
    // Throws std::invalid_argument for exchange settings Exchange rejects.
    explicit Simulation(std::uint64_t seed, const ExchangeConfig& exchange = {});

    // Opens an account for the agent, schedules its start and returns its id: 1 for the first
    // agent, then 2, and so on. Throws std::invalid_argument if the agent is null, a latency is
    // negative, the start time has passed or the account limits are invalid.
    AgentId addAgent(std::unique_ptr<Agent> agent, const AgentOptions& options = {});

    // Reports every request reaching the exchange and every event it produces to `sink`, which is
    // not owned. Pass nullptr to stop.
    void setEventSink(EventSink* sink) noexcept { sink_ = sink; }

    // Moves the exchange to `phase` at `time`, as a scheduled action like any other: everything
    // it produces is published and logged then. A halt schedules its own end, after the
    // exchange's halt duration, which takes effect only if the market is still halted then.
    // Throws std::invalid_argument for a time in the past.
    void schedulePhase(Timestamp time, Phase phase);

    // Processes everything scheduled up to and including `endTime`, then moves the clock there.
    void runUntil(Timestamp endTime);

    // Runs `action` with the agent's context at the current time, as if one of its callbacks
    // were running: whatever it sends leaves now. This is how an agent driven from outside the
    // simulation, such as a person trading live, acts between calls to runUntil. Throws
    // std::out_of_range for an unknown id.
    void act(AgentId id, const std::function<void(AgentContext&)>& action);

    [[nodiscard]] Timestamp now() const noexcept { return now_; }
    // Messages and wakeups scheduled but not yet processed.
    [[nodiscard]] std::size_t pendingCount() const noexcept { return queue_.size(); }
    // When the next of them is due, or nullopt if none is.
    [[nodiscard]] std::optional<Timestamp> nextEventTime() const noexcept {
        return queue_.empty() ? std::nullopt : std::optional{queue_.front().time};
    }
    [[nodiscard]] const Exchange& exchange() const noexcept { return exchange_; }
    // These throw std::out_of_range for an unknown id.
    [[nodiscard]] Agent& agent(AgentId id);
    [[nodiscard]] const Ledger& ledger(AgentId id) const;
    // The public market as the agent can see it now, one fromExchange latency late, exactly as
    // its context's market() would show it.
    [[nodiscard]] MarketSnapshot marketSeenBy(AgentId id) const;

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
    // The exchange moving to another phase. The end of a halt applies only to a halted market.
    struct PhaseAction {
        Phase phase = Phase::Continuous;
        bool endsHalt = false;
    };
    using Action = std::variant<Start, Wakeup, Arrival, Delivery, PhaseAction>;

    // An entry in the queue: when, in what order among equal times, and where its action is kept.
    // The heap moves entries constantly, so they stay small and trivially copyable, and the
    // actions, which can hold vectors, stay put.
    struct Scheduled {
        Timestamp time = 0;
        std::uint64_t sequence = 0;
        std::uint32_t action = 0; // index into actions_
    };

    // The exchange's public state from `time` until the next change.
    struct PublicState {
        Timestamp time = 0;
        MarketSnapshot market{};
    };

    // Queues an action for `time` and returns its slot, for the caller to build the action in.
    Action& schedule(Timestamp time);
    void send(AgentId sender, Request request);
    void deliver(AgentId recipient, const Event& event);
    Duration drawJitter(Slot& endpoint);
    Slot& slot(AgentId id);
    void process(const Start& start);
    void process(const Wakeup& wakeup);
    void process(const Arrival& arrival);
    void process(const Delivery& delivery);
    void process(const PhaseAction& action);
    // Logs, delivers and publishes the events in events_, and schedules the end of a halt that
    // one of them starts.
    void dispatch();
    void recordPublicState();
    [[nodiscard]] MarketSnapshot visibleMarket(AgentId id) const;

    std::uint64_t seed_;
    Exchange exchange_;
    std::vector<Slot> slots_;
    std::vector<Scheduled> queue_; // a binary heap with the earliest item at the front
    // What each queued entry does. A deque, so that an action being processed stays where it is
    // while processing it schedules more; a slot is reused once its action is done.
    std::deque<Action> actions_;
    std::vector<std::uint32_t> freeActions_;
    std::uint64_t nextSequence_ = 0;
    Timestamp now_ = 0;
    EventSink* sink_ = nullptr;
    std::vector<Event> events_; // reused for every request

    std::vector<AgentId> streamed_; // agents sent every public update
    MarketSnapshot published_;      // the exchange's public state now
    // Public states some agent could still be shown, oldest first. Nobody looks further back than
    // the largest fromExchange latency, so older states are dropped and the history stays short.
    detail::Ring<PublicState> history_;
    Duration longestDelay_ = 0;
};

} // namespace crowdbook

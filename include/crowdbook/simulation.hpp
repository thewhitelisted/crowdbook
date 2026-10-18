#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "crowdbook/agent.hpp"
#include "crowdbook/detail/event_queue.hpp"
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
    // A link's delay as a measured distribution rather than one number: when not empty, each
    // message on that link takes a delay drawn afresh from these quantiles, which are in order and
    // evenly spaced in probability from the shortest to the longest: one of the gaps between
    // neighbours, each as likely, then uniformly within it. toExchange or fromExchange is taken to
    // be their median, which is what an agent that looks at the market on demand sees it with.
    // Jitter adds to either.
    std::vector<Duration> toExchangeQuantiles{};
    std::vector<Duration> fromExchangeQuantiles{};

    // The longest a message from the exchange can take, jitter included.
    [[nodiscard]] Duration longestFromExchange() const noexcept;

    friend bool operator==(const Latency&, const Latency&) = default;
};

// The median of quantiles evenly spaced in probability, halfway between the middle two when there
// is an even number of them; 0 for none.
[[nodiscard]] Duration medianOf(const std::vector<Duration>& quantiles) noexcept;

struct AgentOptions {
    AccountConfig account{};
    Latency latency{};
    std::optional<Timestamp> startTime{}; // when onStart runs; the current time if unset
};

// The discrete-event kernel. It owns the exchange and the agents and processes every message and
// wakeup in time order; items due at the same time run in the order they were scheduled. Each
// agent draws from its own random streams, so a run is fully determined by its seed. Messages on
// one link (an agent to the exchange, or the exchange to an agent) never overtake each other, even
// with jitter or delays drawn from a distribution: a message held up holds up those behind it, as
// on one connection. The exchange itself takes no time to process a request.
// New orders and amendments held at the exchange before they reach the book, for a stretch of the
// run, as betting exchanges hold bets while an event is in play: one that arrives from `from` until
// `until` waits `delay` more, and is accepted, or rejected, only then. Cancels are not held, so a
// resting order can still be cancelled at once.
struct OrderDelay {
    Timestamp from = 0;
    Timestamp until = 0;
    Duration delay = 0;

    friend bool operator==(const OrderDelay&, const OrderDelay&) = default;
};

class Simulation {
public:
    // Throws std::invalid_argument for exchange settings Exchange rejects.
    explicit Simulation(std::uint64_t seed, const ExchangeConfig& exchange = {});

    // Opens an account for the agent, schedules its start and returns its id: 1 for the first
    // agent, then 2, and so on. A link with quantiles takes their median as its delay where one
    // number is needed. Throws std::invalid_argument if the agent is null, a latency is negative,
    // quantiles are out of order, the start time has passed or the account limits are invalid.
    AgentId addAgent(std::unique_ptr<Agent> agent, const AgentOptions& options = {});

    // Reports every request reaching the exchange and every event it produces to `sink`, which is
    // not owned. Pass nullptr to stop.
    void setEventSink(EventSink* sink) noexcept { sink_ = sink; }

    // Moves the exchange to `phase` at `time`, as a scheduled action like any other: everything
    // it produces is published and logged then. A halt schedules its own end, after the
    // exchange's halt duration, which takes effect only if the market is still halted then.
    // Throws std::invalid_argument for a time in the past.
    void schedulePhase(Timestamp time, Phase phase);

    // Holds new orders and amendments that arrive in the delay's stretch of the run. Throws
    // std::invalid_argument for a negative delay or a stretch that ends before it starts.
    void setOrderDelay(const OrderDelay& delay);

    // Processes everything scheduled up to and including `endTime`, then moves the clock there.
    // Throws std::invalid_argument for a time after kLatestTime.
    //
    // An exception that escapes an agent, the exchange or the event sink while the simulation
    // runs, such as std::bad_alloc, leaves every structure valid, so the simulation can still be
    // inspected and destroyed safely, but the market may be half way through an event, so from
    // then on runUntil and act throw std::logic_error rather than run on.
    void runUntil(Timestamp endTime);

    // Called from inside a run, by an agent or a sink, makes runUntil return once everything due
    // at the current time has run, with the clock left there instead of at its end time. Running
    // on from there processes the same items in the same order as a run that was never stopped,
    // so a driver can stop the market when something happens to its agent, act, and carry on.
    // Outside a run it does nothing.
    void interrupt() noexcept { interrupted_ = true; }

    // Runs `action` with the agent's context at the current time, as if one of its callbacks
    // were running: whatever it sends leaves now. This is how an agent driven from outside the
    // simulation, such as a person trading live, acts between calls to runUntil. Throws
    // std::out_of_range for an unknown id. An exception that escapes the action stops the
    // simulation, as one escaping runUntil does.
    void act(AgentId id, const std::function<void(AgentContext&)>& action);

    // Whether an exception escaped runUntil or act, after which the simulation runs no further.
    // Read from inside a run, by an agent or a sink, it is true, as the run has not finished.
    [[nodiscard]] bool failed() const noexcept { return failed_; }

    [[nodiscard]] Timestamp now() const noexcept { return now_; }
    // Messages and wakeups scheduled but not yet processed.
    [[nodiscard]] std::size_t pendingCount() const noexcept { return queue_.size(); }
    // When the next of them is due, or nullopt if none is.
    [[nodiscard]] std::optional<Timestamp> nextEventTime() const noexcept {
        return queue_.peekTime();
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
        Random network{0, 0}; // draws the jitter and drawn delays on the agent's links
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
        bool held = false; // already held by the order delay, now reaching the book
    };
    // An event reaching an agent: one of its own, which its ledger records, or a public one.
    struct Delivery {
        AgentId recipient = 0;
        Event event{};
        bool own = false;
    };
    // The exchange moving to another phase. The end of a halt applies only to a halted market.
    struct PhaseAction {
        Phase phase = Phase::Continuous;
        bool endsHalt = false;
    };
    using Action = std::variant<Start, Wakeup, Arrival, Delivery, PhaseAction>;

    // The exchange's public state from `time` until the next change.
    struct PublicState {
        Timestamp time = 0;
        MarketSnapshot market{};
    };

    // Queues an action for `time` and returns its slot, for the caller to build the action in.
    Action& schedule(Timestamp time);
    void throwIfFailed() const {
        if (failed_) [[unlikely]] {
            throwFailed();
        }
    }
    [[noreturn]] void throwFailed() const;
    void send(AgentId sender, Request request);
    void deliver(AgentId recipient, const Event& event, bool own);
    // A message's delay on one of the agent's links: the link's own, or one drawn from its
    // quantiles, and the jitter.
    Duration drawDelay(Slot& endpoint, bool toExchange);
    // Throws std::out_of_range for an unknown id.
    Slot& slot(AgentId id);
    // For an id the simulation itself handed out or scheduled, which is always known.
    Slot& knownSlot(AgentId id) noexcept { return slots_[id - 1]; }
    [[nodiscard]] const Slot& knownSlot(AgentId id) const noexcept { return slots_[id - 1]; }
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
    // What is scheduled, earliest first. An action being processed stays where it is while
    // processing it schedules more, and its slot is reused only once it is done.
    detail::EventQueue<Action> queue_;
    Timestamp now_ = 0;
    bool failed_ = false; // set while a run is under way, and left set if an exception escapes
    bool interrupted_ = false; // asked to stop after the current time
    EventSink* sink_ = nullptr;
    OrderDelay orderDelay_{};
    std::vector<Event> events_; // reused for every request

    std::vector<AgentId> streamed_; // agents sent every public update
    MarketSnapshot published_;      // the exchange's public state now
    // Public states some agent could still be shown, oldest first. Nobody looks further back than
    // the largest fromExchange latency, so older states are dropped and the history stays short.
    detail::Ring<PublicState> history_;
    Duration longestDelay_ = 0;
};

} // namespace crowdbook

#include "crowdbook/simulation.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "overloaded.hpp"

namespace crowdbook {

namespace {

// Heap order: the earliest time on top, and among equal times the first scheduled.
struct Later {
    template <typename Item>
    bool operator()(const Item& a, const Item& b) const noexcept {
        return a.time != b.time ? a.time > b.time : a.sequence > b.sequence;
    }
};

} // namespace

// The context handed to an agent's callbacks. It only forwards to the simulation, so agents can
// act but never reach the exchange or another agent directly.
class Simulation::Context final : public AgentContext {
public:
    Context(Simulation& simulation, AgentId agent) noexcept
        : simulation_(simulation), agent_(agent) {}

    [[nodiscard]] AgentId id() const noexcept override { return agent_; }
    [[nodiscard]] Timestamp now() const noexcept override { return simulation_.now_; }
    [[nodiscard]] Random& random() noexcept override { return simulation_.slot(agent_).random; }
    [[nodiscard]] const Ledger& ledger() const noexcept override {
        return simulation_.slot(agent_).ledger;
    }
    [[nodiscard]] MarketSnapshot market() const override {
        return simulation_.visibleMarket(agent_);
    }
    [[nodiscard]] Price maxPrice() const noexcept override {
        return simulation_.exchange_.config().maxPrice;
    }

    ClientOrderId submit(NewOrder order) override {
        order.clientOrderId = simulation_.slot(agent_).nextClientOrderId++;
        simulation_.send(agent_, order);
        return order.clientOrderId;
    }

    void cancel(ClientOrderId clientOrderId) override {
        simulation_.send(agent_, CancelOrder{.clientOrderId = clientOrderId});
    }

    void modify(ClientOrderId clientOrderId, Price price, Quantity quantity) override {
        simulation_.send(agent_, ModifyOrder{.clientOrderId = clientOrderId,
                                             .price = price,
                                             .quantity = quantity});
    }

    void wakeAt(Timestamp time, std::uint64_t tag) override {
        simulation_.schedule(std::max(time, simulation_.now_)).emplace<Wakeup>(agent_, tag);
    }

private:
    Simulation& simulation_;
    AgentId agent_;
};

Simulation::Simulation(std::uint64_t seed, const ExchangeConfig& exchange)
    : seed_(seed), exchange_(exchange) {}

AgentId Simulation::addAgent(std::unique_ptr<Agent> agent, const AgentOptions& options) {
    if (!agent) {
        throw std::invalid_argument("agent must not be null");
    }
    const Latency& latency = options.latency;
    const auto inRange = [](Duration part) { return part >= 0 && part <= kMaxDuration; };
    if (!inRange(latency.toExchange) || !inRange(latency.fromExchange) ||
        !inRange(latency.jitter)) {
        throw std::invalid_argument("latencies must be from 0 to 1000000000s");
    }
    const Timestamp startTime = options.startTime.value_or(now_);
    if (startTime < now_) {
        throw std::invalid_argument("the start time has already passed");
    }

    const auto id = static_cast<AgentId>(slots_.size() + 1);
    const MarketDataMode marketData = agent->marketData();
    exchange_.addAgent(id, options.account);
    // Stream 2 * id feeds the agent's own draws and 2 * id + 1 its network jitter, so neither
    // depends on how many agents there are or on what the other draws.
    slots_.push_back(Slot{
        .agent = std::move(agent),
        .latency = latency,
        .random = Random{seed_, 2 * std::uint64_t{id}},
        .network = Random{seed_, 2 * std::uint64_t{id} + 1},
        .ledger = Ledger{options.account.initialCash, options.account.initialPosition},
    });
    if (marketData == MarketDataMode::Stream) {
        streamed_.push_back(id);
    }
    // An agent added mid-run with a longer delay than anyone before it sees nothing from before it
    // joined that has already been dropped; it sees the oldest state still kept instead.
    longestDelay_ = std::max(longestDelay_, latency.fromExchange);
    schedule(startTime).emplace<Start>(id);
    return id;
}

void Simulation::runUntil(Timestamp endTime) {
    throwIfFailed();
    if (endTime > kLatestTime) {
        throw std::invalid_argument(
            std::format("the simulation cannot run past {} ns, about 127 years", kLatestTime));
    }
    // Cleared only if the run gets to the end: an exception that escapes leaves it set, and the
    // simulation stopped. Nothing is cleaned up then, since nothing will run again, and the
    // simulation frees everything it holds when it is destroyed.
    failed_ = true;
    while (!queue_.empty() && queue_.front().time <= endTime) {
        std::ranges::pop_heap(queue_, Later{});
        const Scheduled item = queue_.back();
        queue_.pop_back();
        now_ = item.time;
        std::visit([this](const auto& action) { process(action); }, actions_[item.action]);
        freeActions_.push_back(item.action); // only now, as processing may schedule more
    }
    now_ = std::max(now_, endTime);
    failed_ = false;
}

void AgentContext::throwWakeupTooFar() {
    throw std::invalid_argument("a wakeup cannot be more than 1000000000s away");
}

void Simulation::throwFailed() const {
    throw std::logic_error(std::format(
        "the simulation stopped at {} ns, when an error escaped it, and cannot go on", now_));
}

void Simulation::act(AgentId id, const std::function<void(AgentContext&)>& action) {
    throwIfFailed();
    static_cast<void>(slot(id));
    Context context{*this, id};
    failed_ = true; // as in runUntil
    action(context);
    failed_ = false;
}

Agent& Simulation::agent(AgentId id) { return *slot(id).agent; }

MarketSnapshot Simulation::marketSeenBy(AgentId id) const {
    if (id == 0 || id > slots_.size()) {
        throw std::out_of_range(std::format("no agent with id {}", id));
    }
    return visibleMarket(id);
}

const Ledger& Simulation::ledger(AgentId id) const {
    if (id == 0 || id > slots_.size()) {
        throw std::out_of_range(std::format("no agent with id {}", id));
    }
    return slots_[id - 1].ledger;
}

Simulation::Action& Simulation::schedule(Timestamp time) {
    static_assert(std::is_trivially_copyable_v<Scheduled>);
    std::uint32_t slot = 0;
    if (freeActions_.empty()) {
        slot = static_cast<std::uint32_t>(actions_.size());
        actions_.emplace_back();
    } else {
        slot = freeActions_.back();
        freeActions_.pop_back();
    }
    queue_.push_back(Scheduled{.time = time, .sequence = nextSequence_++, .action = slot});
    std::ranges::push_heap(queue_, Later{});
    return actions_[slot];
}

void Simulation::send(AgentId sender, Request request) {
    Slot& from = slot(sender);
    from.ledger.recordRequest(request);
    const Timestamp arrival = std::max(now_ + from.latency.toExchange + drawJitter(from),
                                       from.lastArrivalAtExchange);
    from.lastArrivalAtExchange = arrival;
    schedule(arrival).emplace<Arrival>(sender, std::move(request));
}

void Simulation::deliver(AgentId recipient, const Event& event) {
    Slot& to = slot(recipient);
    const Timestamp arrival = std::max(now_ + to.latency.fromExchange + drawJitter(to),
                                       to.lastArrivalAtAgent);
    to.lastArrivalAtAgent = arrival;
    schedule(arrival).emplace<Delivery>(recipient, event);
}

Duration Simulation::drawJitter(Slot& endpoint) {
    const Duration jitter = endpoint.latency.jitter;
    return jitter == 0 ? 0 : endpoint.network.uniformInt(0, jitter);
}

Simulation::Slot& Simulation::slot(AgentId id) {
    if (id == 0 || id > slots_.size()) {
        throw std::out_of_range(std::format("no agent with id {}", id));
    }
    return slots_[id - 1];
}

void Simulation::process(const Start& start) {
    Context context{*this, start.agent};
    slot(start.agent).agent->onStart(context);
}

void Simulation::process(const Wakeup& wakeup) {
    Context context{*this, wakeup.agent};
    slot(wakeup.agent).agent->onWakeup(context, wakeup.tag);
}

void Simulation::schedulePhase(Timestamp time, Phase phase) {
    if (time < now_) {
        throw std::invalid_argument(
            std::format("a phase change at {} ns is in the past: it is {} ns", time, now_));
    }
    schedule(time).emplace<PhaseAction>(PhaseAction{.phase = phase});
}

void Simulation::process(const PhaseAction& action) {
    if (action.endsHalt && exchange_.phase() != Phase::HaltAuction) {
        return; // the closing auction, or the close, came first
    }
    events_.clear();
    exchange_.setPhase(action.phase, events_);
    dispatch();
}

void Simulation::process(const Arrival& arrival) {
    if (sink_ != nullptr) {
        sink_->onRequest(now_, arrival.sender, arrival.request);
    }
    events_.clear();
    exchange_.handle(arrival.sender, arrival.request, events_);
    dispatch();
}

void Simulation::dispatch() {
    bool marketChanged = false;
    for (const Event& event : events_) {
        if (sink_ != nullptr) {
            sink_->onEvent(now_, event);
        }
        if (const std::optional<AgentId> to = recipient(event)) {
            deliver(*to, event);
            continue;
        }
        if (const auto* trade = std::get_if<Trade>(&event)) {
            published_.lastTrade = trade->price;
            ++published_.trades;
            published_.volume += trade->quantity;
        } else if (const auto* top = std::get_if<TopOfBook>(&event)) {
            published_.bid = top->bid;
            published_.ask = top->ask;
        } else if (const auto* depth = std::get_if<BookDepth>(&event)) {
            published_.bids = depth->bids;
            published_.asks = depth->asks;
        } else if (const auto* phase = std::get_if<PhaseChanged>(&event)) {
            published_.phase = phase->phase;
            if (!isAuction(phase->phase)) {
                published_.indicative.reset();
            }
            if (phase->phase == Phase::HaltAuction) {
                schedule(now_ + exchange_.config().haltDuration)
                    .emplace<PhaseAction>(
                        PhaseAction{.phase = Phase::Continuous, .endsHalt = true});
            }
        } else if (const auto* indicative = std::get_if<Indicative>(&event)) {
            published_.indicative = indicative->uncross;
        }
        marketChanged = true;
        for (const AgentId id : streamed_) {
            deliver(id, event);
        }
    }
    if (marketChanged) {
        recordPublicState();
    }
}

void Simulation::recordPublicState() {
    if (!history_.empty() && history_.back().time == now_) {
        history_.back().market = published_;
    } else {
        history_.pushBack(PublicState{.time = now_, .market = published_});
    }
    // Every agent looks at least as recent as now - longestDelay_, so only the last state from
    // before that moment and everything after it can still be needed.
    const Timestamp oldestNeeded = now_ - longestDelay_;
    while (history_.size() >= 2 && history_[1].time <= oldestNeeded) {
        history_.popFront();
    }
}

MarketSnapshot Simulation::visibleMarket(AgentId id) const {
    const Timestamp seenAt = now_ - slots_.at(id - 1).latency.fromExchange;
    // The last state from no later than seenAt, by binary search.
    std::size_t low = 0;
    std::size_t high = history_.size();
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (history_[middle].time <= seenAt) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low == 0 ? MarketSnapshot{} : history_[low - 1].market;
}

void Simulation::process(const Delivery& delivery) {
    Slot& to = slot(delivery.recipient);
    if (recipient(delivery.event)) {
        to.ledger.apply(delivery.event);
    }
    Context context{*this, delivery.recipient};
    Agent& agent = *to.agent;
    std::visit(
        detail::Overloaded{
            [&](const OrderAccepted& event) { agent.onAccepted(context, event); },
            [&](const OrderRejected& event) { agent.onRejected(context, event); },
            [&](const OrderModified& event) { agent.onModified(context, event); },
            [&](const OrderFilled& event) { agent.onFilled(context, event); },
            [&](const OrderCancelled& event) { agent.onCancelled(context, event); },
            [&](const Trade& trade) { agent.onTrade(context, trade); },
            [&](const TopOfBook& top) { agent.onTopOfBook(context, top); },
            [&](const BookDepth& depth) { agent.onDepth(context, depth); },
            [&](const PhaseChanged& phase) { agent.onPhase(context, phase); },
            [&](const Indicative& indicative) { agent.onIndicative(context, indicative); },
        },
        delivery.event);
}

} // namespace crowdbook

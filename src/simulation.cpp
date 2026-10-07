#include "crowdbook/simulation.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>
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
        simulation_.schedule(std::max(time, simulation_.now_), Wakeup{.agent = agent_, .tag = tag});
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
    if (latency.toExchange < 0 || latency.fromExchange < 0 || latency.jitter < 0) {
        throw std::invalid_argument("latencies must not be negative");
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
    schedule(startTime, Start{.agent = id});
    return id;
}

void Simulation::runUntil(Timestamp endTime) {
    while (!queue_.empty() && queue_.front().time <= endTime) {
        std::ranges::pop_heap(queue_, Later{});
        Scheduled item = std::move(queue_.back());
        queue_.pop_back();
        now_ = item.time;
        std::visit([this](const auto& action) { process(action); }, item.action);
    }
    now_ = std::max(now_, endTime);
}

void Simulation::act(AgentId id, const std::function<void(AgentContext&)>& action) {
    static_cast<void>(slot(id));
    Context context{*this, id};
    action(context);
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

void Simulation::schedule(Timestamp time, Action action) {
    queue_.push_back(
        Scheduled{.time = time, .sequence = nextSequence_++, .action = std::move(action)});
    std::ranges::push_heap(queue_, Later{});
}

void Simulation::send(AgentId sender, Request request) {
    Slot& from = slot(sender);
    from.ledger.recordRequest(request);
    const Timestamp arrival = std::max(now_ + from.latency.toExchange + drawJitter(from),
                                       from.lastArrivalAtExchange);
    from.lastArrivalAtExchange = arrival;
    schedule(arrival, Arrival{.sender = sender, .request = std::move(request)});
}

void Simulation::deliver(AgentId recipient, const Event& event) {
    Slot& to = slot(recipient);
    const Timestamp arrival = std::max(now_ + to.latency.fromExchange + drawJitter(to),
                                       to.lastArrivalAtAgent);
    to.lastArrivalAtAgent = arrival;
    schedule(arrival, Delivery{.recipient = recipient, .event = event});
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

void Simulation::process(const Arrival& arrival) {
    if (sink_ != nullptr) {
        sink_->onRequest(now_, arrival.sender, arrival.request);
    }
    events_.clear();
    exchange_.handle(arrival.sender, arrival.request, events_);
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
        history_.push_back(PublicState{.time = now_, .market = published_});
    }
    // Every agent looks at least as recent as now - longestDelay_, so only the last state from
    // before that moment and everything after it can still be needed.
    const Timestamp oldestNeeded = now_ - longestDelay_;
    while (history_.size() >= 2 && history_[1].time <= oldestNeeded) {
        history_.pop_front();
    }
}

MarketSnapshot Simulation::visibleMarket(AgentId id) const {
    const Timestamp seenAt = now_ - slots_.at(id - 1).latency.fromExchange;
    const auto after =
        std::ranges::upper_bound(history_, seenAt, std::less<>{}, &PublicState::time);
    return after == history_.begin() ? MarketSnapshot{} : std::prev(after)->market;
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
        },
        delivery.event);
}

} // namespace crowdbook

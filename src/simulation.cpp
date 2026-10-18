#include "crowdbook/simulation.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>

#include "overloaded.hpp"

namespace crowdbook {

// The context handed to an agent's callbacks. It only forwards to the simulation, so agents can
// act but never reach the exchange or another agent directly.
class Simulation::Context final : public AgentContext {
public:
    Context(Simulation& simulation, AgentId agent) noexcept
        : simulation_(simulation), agent_(agent) {}

    [[nodiscard]] AgentId id() const noexcept override { return agent_; }
    [[nodiscard]] Timestamp now() const noexcept override { return simulation_.now_; }
    [[nodiscard]] Random& random() noexcept override {
        return simulation_.knownSlot(agent_).random;
    }
    [[nodiscard]] const Ledger& ledger() const noexcept override {
        return simulation_.knownSlot(agent_).ledger;
    }
    [[nodiscard]] MarketSnapshot market() const override {
        return simulation_.visibleMarket(agent_);
    }
    [[nodiscard]] Price maxPrice() const noexcept override {
        return simulation_.exchange_.config().maxPrice;
    }

    ClientOrderId submit(NewOrder order) override {
        order.clientOrderId = simulation_.knownSlot(agent_).nextClientOrderId++;
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

Duration Latency::longestFromExchange() const noexcept {
    const Duration longest =
        fromExchangeQuantiles.empty() ? fromExchange : fromExchangeQuantiles.back();
    return longest + jitter;
}

Duration medianOf(const std::vector<Duration>& quantiles) noexcept {
    if (quantiles.empty()) {
        return 0;
    }
    const std::size_t middle = quantiles.size() / 2;
    if (quantiles.size() % 2 == 1) {
        return quantiles[middle];
    }
    const Duration low = quantiles[middle - 1];
    return low + (quantiles[middle] - low) / 2;
}

Simulation::Simulation(std::uint64_t seed, const ExchangeConfig& exchange)
    : seed_(seed), exchange_(exchange) {}

AgentId Simulation::addAgent(std::unique_ptr<Agent> agent, const AgentOptions& options) {
    if (!agent) {
        throw std::invalid_argument("agent must not be null");
    }
    Latency latency = options.latency;
    const auto inRange = [](Duration part) { return part >= 0 && part <= kMaxDuration; };
    const auto allInRange = [&](const std::vector<Duration>& quantiles) {
        return std::ranges::all_of(quantiles, inRange);
    };
    if (!inRange(latency.toExchange) || !inRange(latency.fromExchange) ||
        !inRange(latency.jitter) || !allInRange(latency.toExchangeQuantiles) ||
        !allInRange(latency.fromExchangeQuantiles)) {
        throw std::invalid_argument("latencies must be from 0 to 1000000000s");
    }
    if (!std::ranges::is_sorted(latency.toExchangeQuantiles) ||
        !std::ranges::is_sorted(latency.fromExchangeQuantiles)) {
        throw std::invalid_argument("a latency's quantiles must be in order, shortest first");
    }
    if (!latency.toExchangeQuantiles.empty()) {
        latency.toExchange = medianOf(latency.toExchangeQuantiles);
    }
    if (!latency.fromExchangeQuantiles.empty()) {
        latency.fromExchange = medianOf(latency.fromExchangeQuantiles);
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
    interrupted_ = false;
    while (!queue_.empty()) {
        const Timestamp next = queue_.nextTime();
        if (next > endTime || (interrupted_ && next > now_)) {
            break;
        }
        const auto item = queue_.pop();
        now_ = item.time;
        std::visit([this](const auto& action) { process(action); }, queue_.payload(item.slot));
        queue_.release(item.slot); // only now, as processing may schedule more
    }
    if (!interrupted_) {
        now_ = std::max(now_, endTime);
    }
    interrupted_ = false;
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

Simulation::Action& Simulation::schedule(Timestamp time) { return queue_.push(time); }

void Simulation::send(AgentId sender, Request request) {
    Slot& from = knownSlot(sender);
    from.ledger.recordRequest(request);
    const Timestamp arrival = std::max(now_ + drawDelay(from, true), from.lastArrivalAtExchange);
    from.lastArrivalAtExchange = arrival;
    schedule(arrival).emplace<Arrival>(sender, std::move(request));
}

void Simulation::deliver(AgentId recipient, const Event& event, bool own) {
    Slot& to = knownSlot(recipient);
    const Timestamp arrival = std::max(now_ + drawDelay(to, false), to.lastArrivalAtAgent);
    to.lastArrivalAtAgent = arrival;
    schedule(arrival).emplace<Delivery>(recipient, event, own);
}

Duration Simulation::drawDelay(Slot& endpoint, bool toExchange) {
    const Latency& latency = endpoint.latency;
    const std::vector<Duration>& quantiles =
        toExchange ? latency.toExchangeQuantiles : latency.fromExchangeQuantiles;
    Duration delay = toExchange ? latency.toExchange : latency.fromExchange;
    if (quantiles.size() == 1) {
        delay = quantiles.front();
    } else if (quantiles.size() > 1) {
        // Evenly spaced in probability, so each gap between neighbours is as likely, and within
        // one the delay is uniform: the distribution the quantiles describe, joined by straight
        // lines. Whole numbers throughout, so every platform draws the same.
        const auto gap = static_cast<std::size_t>(
            endpoint.network.uniformInt(0, static_cast<std::int64_t>(quantiles.size()) - 2));
        delay = endpoint.network.uniformInt(quantiles[gap], quantiles[gap + 1]);
    }
    const Duration jitter = latency.jitter;
    return jitter == 0 ? delay : delay + endpoint.network.uniformInt(0, jitter);
}

Simulation::Slot& Simulation::slot(AgentId id) {
    if (id == 0 || id > slots_.size()) {
        throw std::out_of_range(std::format("no agent with id {}", id));
    }
    return slots_[id - 1];
}

void Simulation::process(const Start& start) {
    Context context{*this, start.agent};
    knownSlot(start.agent).agent->onStart(context);
}

void Simulation::process(const Wakeup& wakeup) {
    Context context{*this, wakeup.agent};
    knownSlot(wakeup.agent).agent->onWakeup(context, wakeup.tag);
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

void Simulation::scheduleClear(Timestamp time) {
    if (time < now_) {
        throw std::invalid_argument("a clear of the book cannot be scheduled in the past");
    }
    schedule(time).emplace<Clear>();
}

void Simulation::process(const Clear& /*clear*/) {
    events_.clear();
    exchange_.clearBook(events_);
    dispatch();
}

void Simulation::setOrderDelay(const OrderDelay& delay) {
    if (delay.delay < 0 || delay.until < delay.from) {
        throw std::invalid_argument(
            "an order delay must not be negative and its stretch must not end before it starts");
    }
    orderDelay_ = delay;
}

void Simulation::process(const Arrival& arrival) {
    if (!arrival.held && (orderDelay_.delay > 0 || !heldOrders_.empty())) {
        const ClientOrderId target =
            std::visit([](const auto& request) { return request.clientOrderId; }, arrival.request);
        const auto key = std::pair{arrival.sender, target};
        Timestamp release = now_;
        if (orderDelay_.delay > 0 && now_ >= orderDelay_.from && now_ < orderDelay_.until &&
            (orderDelay_.cancels || !std::holds_alternative<CancelOrder>(arrival.request))) {
            release = now_ + orderDelay_.delay;
        }
        // A request for an order still held waits for it, so that it never overtakes the order
        // it is about, as on one connection; requests for other orders do not.
        if (const auto found = heldOrders_.find(key); found != heldOrders_.end()) {
            release = std::max(release, found->second);
        }
        if (release > now_) {
            if (!std::holds_alternative<CancelOrder>(arrival.request)) {
                heldOrders_[key] = release;
            }
            // Copied out first: scheduling may move the action being processed.
            Arrival held{.sender = arrival.sender, .request = arrival.request, .held = true};
            schedule(release).emplace<Arrival>(std::move(held));
            return;
        }
    }
    if (arrival.held) {
        const ClientOrderId target =
            std::visit([](const auto& request) { return request.clientOrderId; }, arrival.request);
        if (const auto found = heldOrders_.find(std::pair{arrival.sender, target});
            found != heldOrders_.end() && found->second <= now_) {
            heldOrders_.erase(found);
        }
    }
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
            deliver(*to, event, true);
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
            deliver(id, event, false);
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
    const Timestamp seenAt = now_ - knownSlot(id).latency.fromExchange;
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
    Slot& to = knownSlot(delivery.recipient);
    if (delivery.own) {
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

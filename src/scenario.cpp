#include "crowdbook/scenario.hpp"

#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <variant>

namespace crowdbook {

namespace {

// Counts trades and remembers the last price while passing everything on to another sink.
class TradeCounter final : public EventSink {
public:
    explicit TradeCounter(EventSink* next) noexcept : next_(next) {}

    void onRequest(Timestamp time, AgentId agent, const Request& request) override {
        if (next_ != nullptr) {
            next_->onRequest(time, agent, request);
        }
    }

    void onEvent(Timestamp time, const Event& event) override {
        if (const auto* trade = std::get_if<Trade>(&event)) {
            ++trades_;
            volume_ += trade->quantity;
            lastPrice_ = trade->price;
        }
        if (next_ != nullptr) {
            next_->onEvent(time, event);
        }
    }

    [[nodiscard]] std::uint64_t trades() const noexcept { return trades_; }
    [[nodiscard]] Quantity volume() const noexcept { return volume_; }
    [[nodiscard]] std::optional<Price> lastPrice() const noexcept { return lastPrice_; }

private:
    EventSink* next_;
    std::uint64_t trades_ = 0;
    Quantity volume_ = 0;
    std::optional<Price> lastPrice_;
};

void validate(const Scenario& scenario) {
    if (scenario.duration < 0) {
        throw ScenarioError("the duration must not be negative");
    }
    if (scenario.referencePrice < 1 || scenario.referencePrice > kMaxPrice) {
        throw ScenarioError(
            std::format("the reference price must be between 1 and {}", kMaxPrice));
    }
    if (scenario.groups.empty()) {
        throw ScenarioError("the scenario has no agents");
    }
}

} // namespace

RunResult runScenario(const Scenario& scenario, const AgentRegistry& registry,
                      EventSink* sink) {
    validate(scenario);
    Environment environment{.referencePrice = scenario.referencePrice};
    if (scenario.fundamental) {
        try {
            // Stream 0 belongs to no agent: agent n draws from streams 2n and 2n + 1.
            environment.fundamental =
                std::make_shared<Fundamental>(*scenario.fundamental, Random{scenario.seed, 0});
        } catch (const std::invalid_argument& error) {
            throw ScenarioError(std::format("fundamental: {}", error.what()));
        }
    }

    Simulation simulation{scenario.seed};
    TradeCounter counter{sink};
    simulation.setEventSink(&counter);

    RunResult result;
    result.groups.reserve(scenario.groups.size());
    for (std::size_t index = 0; index < scenario.groups.size(); ++index) {
        const AgentGroup& group = scenario.groups[index];
        GroupResult& summary = result.groups.emplace_back(GroupResult{
            .name = group.name.empty() ? group.type : group.name, .type = group.type});
        try {
            if (group.count < 1) {
                throw std::invalid_argument("count must be at least 1");
            }
            for (std::int64_t i = 0; i < group.count; ++i) {
                summary.agents.push_back(simulation.addAgent(
                    registry.create(group.type, group.parameters, environment), group.options));
            }
        } catch (const std::invalid_argument& error) {
            throw ScenarioError(
                std::format("agent group {} ({}): {}", index + 1, summary.name, error.what()));
        }
    }

    simulation.runUntil(scenario.duration);

    result.trades = counter.trades();
    result.volume = counter.volume();
    result.lastPrice = counter.lastPrice().value_or(scenario.referencePrice);
    if (environment.fundamental) {
        result.finalValue = environment.fundamental->valueAt(scenario.duration);
    }
    for (std::size_t index = 0; index < scenario.groups.size(); ++index) {
        const AccountConfig& start = scenario.groups[index].options.account;
        GroupResult& summary = result.groups[index];
        for (const AgentId id : summary.agents) {
            const Account& account = simulation.exchange().account(id);
            summary.cash += account.cash;
            summary.position += account.position;
        }
        const auto agents = static_cast<std::int64_t>(summary.agents.size());
        summary.pnl = (summary.cash - agents * start.initialCash) +
                      (summary.position - agents * start.initialPosition) * result.lastPrice;
    }
    return result;
}

} // namespace crowdbook

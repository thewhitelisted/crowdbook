#include "crowdbook/scenario.hpp"

#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace crowdbook {

namespace {

// Counts trades and each agent's traded volume, and remembers the last price, while passing
// everything on to another sink.
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
        } else if (const auto* fill = std::get_if<OrderFilled>(&event)) {
            if (fill->agent >= traded_.size()) {
                traded_.resize(fill->agent + std::size_t{1}, 0);
            }
            traded_[fill->agent] += fill->quantity;
        }
        if (next_ != nullptr) {
            next_->onEvent(time, event);
        }
    }

    [[nodiscard]] std::uint64_t trades() const noexcept { return trades_; }
    [[nodiscard]] Quantity volume() const noexcept { return volume_; }
    [[nodiscard]] std::optional<Price> lastPrice() const noexcept { return lastPrice_; }
    [[nodiscard]] Quantity traded(AgentId agent) const noexcept {
        return agent < traded_.size() ? traded_[agent] : 0;
    }

private:
    EventSink* next_;
    std::uint64_t trades_ = 0;
    Quantity volume_ = 0;
    std::optional<Price> lastPrice_;
    std::vector<Quantity> traded_; // by agent id
};

std::string jsonString(std::string_view text) {
    std::string quoted = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            quoted += '\\';
            quoted += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            quoted += std::format("\\u{:04x}", static_cast<unsigned>(c));
        } else {
            quoted += c;
        }
    }
    return quoted + '"';
}

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
    try {
        const Exchange check{scenario.exchange};
    } catch (const std::invalid_argument& error) {
        throw ScenarioError(std::format("exchange: {}", error.what()));
    }
}

} // namespace

struct ScenarioRun::State {
    State(const Scenario& scenario, EventSink* sink)
        : referencePrice(scenario.referencePrice), counter(sink),
          simulation(scenario.seed, scenario.exchange) {
        simulation.setEventSink(&counter);
    }

    Price referencePrice;
    std::shared_ptr<Fundamental> fundamental;
    TradeCounter counter;
    Simulation simulation;
    std::vector<GroupResult> groups;
    std::vector<AccountConfig> startingAccounts; // by group
};

ScenarioRun::ScenarioRun(const Scenario& scenario, const AgentRegistry& registry,
                         EventSink* sink) {
    validate(scenario);
    state_ = std::make_unique<State>(scenario, sink);
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
    state_->fundamental = environment.fundamental;

    state_->groups.reserve(scenario.groups.size());
    for (std::size_t index = 0; index < scenario.groups.size(); ++index) {
        const AgentGroup& group = scenario.groups[index];
        GroupResult& summary = state_->groups.emplace_back(GroupResult{
            .name = group.name.empty() ? group.type : group.name, .type = group.type});
        state_->startingAccounts.push_back(group.options.account);
        try {
            if (group.count < 1) {
                throw std::invalid_argument("count must be at least 1");
            }
            for (std::int64_t i = 0; i < group.count; ++i) {
                summary.agents.push_back(state_->simulation.addAgent(
                    registry.create(group.type, group.parameters, environment), group.options));
            }
        } catch (const std::invalid_argument& error) {
            throw ScenarioError(
                std::format("agent group {} ({}): {}", index + 1, summary.name, error.what()));
        }
    }
}

ScenarioRun::ScenarioRun(ScenarioRun&&) noexcept = default;
ScenarioRun& ScenarioRun::operator=(ScenarioRun&&) noexcept = default;
ScenarioRun::~ScenarioRun() = default;

AgentId ScenarioRun::addAgent(std::string name, std::string type, std::unique_ptr<Agent> agent,
                              const AgentOptions& options) {
    const AgentId id = state_->simulation.addAgent(std::move(agent), options);
    state_->groups.push_back(
        GroupResult{.name = std::move(name), .type = std::move(type), .agents = {id}});
    state_->startingAccounts.push_back(options.account);
    return id;
}

void ScenarioRun::runUntil(Timestamp time) { state_->simulation.runUntil(time); }

Simulation& ScenarioRun::simulation() noexcept { return state_->simulation; }

const Simulation& ScenarioRun::simulation() const noexcept { return state_->simulation; }

RunResult ScenarioRun::result() {
    const TradeCounter& counter = state_->counter;
    RunResult result{.trades = counter.trades(),
                     .volume = counter.volume(),
                     .lastPrice = counter.lastPrice().value_or(state_->referencePrice)};
    if (state_->fundamental) {
        result.finalValue = state_->fundamental->valueAt(state_->simulation.now());
    }
    result.groups.reserve(state_->groups.size());
    for (std::size_t index = 0; index < state_->groups.size(); ++index) {
        const AccountConfig& start = state_->startingAccounts[index];
        GroupResult& summary = result.groups.emplace_back(GroupResult{
            .name = state_->groups[index].name,
            .type = state_->groups[index].type,
            .agents = state_->groups[index].agents});
        for (const AgentId id : summary.agents) {
            const Account& account = state_->simulation.exchange().account(id);
            summary.traded += counter.traded(id);
            summary.cash += account.cash;
            summary.position += account.position;
            summary.fees += account.fees;
        }
        const auto agents = static_cast<std::int64_t>(summary.agents.size());
        summary.initialCash = agents * start.initialCash;
        summary.initialPosition = agents * start.initialPosition;
        summary.pnl = (summary.cash - summary.initialCash) +
                      (summary.position - summary.initialPosition) * result.lastPrice;
    }
    return result;
}

RunResult runScenario(const Scenario& scenario, const AgentRegistry& registry,
                      EventSink* sink) {
    ScenarioRun run{scenario, registry, sink};
    run.runUntil(scenario.duration);
    return run.result();
}

void writeResultJson(std::ostream& out, const Scenario& scenario, const RunResult& result) {
    out << std::format("{{\n  \"seed\": {},\n  \"duration_ns\": {},\n  \"reference_price\": {},\n",
                       scenario.seed, scenario.duration, scenario.referencePrice);
    out << std::format("  \"trades\": {},\n  \"volume\": {},\n  \"last_price\": {},\n",
                       result.trades, result.volume, result.lastPrice);
    out << "  \"final_value\": "
        << (result.finalValue ? std::format("{}", *result.finalValue) : "null") << ",\n";
    out << "  \"groups\": [";
    for (std::size_t index = 0; index < result.groups.size(); ++index) {
        const GroupResult& group = result.groups[index];
        std::string ids;
        for (const AgentId id : group.agents) {
            ids += std::format("{}{}", ids.empty() ? "" : ", ", id);
        }
        out << (index == 0 ? "\n" : ",\n");
        out << std::format("    {{\"name\": {}, \"type\": {}, \"agents\": {}, \"agent_ids\": [{}], "
                           "\"traded\": {}, \"initial_cash\": {}, \"initial_position\": {}, "
                           "\"cash\": {}, \"position\": {}, \"pnl\": {}, \"fees\": {}}}",
                           jsonString(group.name), jsonString(group.type), group.agents.size(),
                           ids, group.traded, group.initialCash, group.initialPosition,
                           group.cash, group.position, group.pnl, formatFee(group.fees));
    }
    out << "\n  ]";
    if (!result.scores.empty()) {
        out << ",\n  \"scores\": [";
        for (std::size_t index = 0; index < result.scores.size(); ++index) {
            const Score& score = result.scores[index].score;
            out << (index == 0 ? "\n" : ",\n");
            out << std::format("    {{\"seat\": {}, \"total\": {}, \"pnl\": {}, \"inventory\": {}, "
                               "\"close\": {}, \"paper\": {}, \"unfinished\": {}, "
                               "\"unfinished_lots\": {}, \"stopped_at_ns\": {}}}",
                               jsonString(result.scores[index].seat), score.total, score.pnl,
                               score.inventory, score.close, score.paper, score.unfinished,
                               score.unfinishedLots,
                               score.stoppedAt ? std::format("{}", *score.stoppedAt) : "null");
        }
        out << "\n  ]";
    }
    out << "\n}\n";
}

} // namespace crowdbook

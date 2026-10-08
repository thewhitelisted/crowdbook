#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/event_log.hpp"
#include "crowdbook/fundamental.hpp"
#include "crowdbook/parameters.hpp"
#include "crowdbook/scoring.hpp"
#include "crowdbook/simulation.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// A malformed or inconsistent scenario. The message says where the problem is.
class ScenarioError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Agents of one type that share their settings. Each still gets its own id and random streams.
struct AgentGroup {
    std::string name{}; // label for results; the type when empty
    std::string type{};
    std::int64_t count = 1;
    AgentOptions options{};   // account, latency and start time, shared by the whole group
    Parameters parameters{};  // everything else, handed to the agent type's factory
};

// A market that trades the way a stock exchange's day goes, in phases.
struct TradingDay {
    Duration openingAuction = 0; // a call auction from the start; none if 0
    Duration closingAuction = 0; // a call auction before the end, uncrossed at the end; none if 0
    // A continuous trade more than haltBand ticks from the last auction's price, or the reference
    // price before any, halts the market into an auction for `halt`. 0 never halts.
    Price haltBand = 0;
    Duration halt = 0;
    double activity = 0.0; // the amplitude of the U-shaped activity curve; flat if 0

    friend bool operator==(const TradingDay&, const TradingDay&) = default;
};

// A scenario offered as a challenge: its name, and what the participant is asked to do.
struct Challenge {
    std::string name{};
    std::string briefing{};

    friend bool operator==(const Challenge&, const Challenge&) = default;
};

// A market to simulate: who trades, with which settings, and for how long.
struct Scenario {
    std::uint64_t seed = 1;
    Duration duration = 60 * kSecond;
    Price referencePrice = 10'000;
    std::optional<FundamentalConfig> fundamental{};
    ExchangeConfig exchange{};
    // Without one, the market trades continuously from start to end.
    std::optional<TradingDay> tradingDay{};
    std::vector<AgentGroup> groups{};
    // The account and latency of a person trading in the market live; unused by runScenario.
    AgentOptions participant{};
    // How live participants are scored, if they are; unused by runScenario.
    std::optional<ScoringConfig> scoring{};
    std::optional<Challenge> challenge{};
};

// Totals over the agents of one group.
struct GroupResult {
    std::string name{};
    std::string type{};
    std::vector<AgentId> agents{};
    Quantity traded = 0; // lots bought and sold
    Cash initialCash = 0;
    Quantity initialPosition = 0;
    Cash cash = 0;
    Quantity position = 0;
    // Change in cash plus change in position valued at the last price, in tick-lots, before fees.
    Cash pnl = 0;
    Fee fees = 0; // paid to the exchange, net of rebates, in fee units
};

// A live participant's score, by the name of its seat.
struct SeatScore {
    std::string seat{};
    Score score{};

    friend bool operator==(const SeatScore&, const SeatScore&) = default;
};

struct RunResult {
    std::uint64_t trades = 0;
    Quantity volume = 0;
    Price lastPrice = 0; // the last trade price, or the reference price if nothing traded
    // The fundamental value at the end, if the scenario has one, to compare with lastPrice.
    std::optional<double> finalValue{};
    std::vector<GroupResult> groups{};
    // Each seat's score, for a session of a scenario with scoring; otherwise empty.
    std::vector<SeatScore> scores{};
};

// A scenario's market, built and ready to run in steps. runScenario builds one and runs it to the
// end; a live session runs it a little at a time and acts in between.
class ScenarioRun {
public:
    // Builds the market. Every request and event also goes to `sink`, if there is one. Throws
    // ScenarioError for settings no market can be built from, naming the agent group at fault.
    ScenarioRun(const Scenario& scenario, const AgentRegistry& registry, EventSink* sink = nullptr);
    ScenarioRun(ScenarioRun&&) noexcept;
    ScenarioRun& operator=(ScenarioRun&&) noexcept;
    ScenarioRun(const ScenarioRun&) = delete;
    ScenarioRun& operator=(const ScenarioRun&) = delete;
    ~ScenarioRun();

    // Adds an agent that is not one of the scenario's groups, such as a person trading live,
    // reported in the results as a group of its own. Throws std::invalid_argument as
    // Simulation::addAgent does.
    AgentId addAgent(std::string name, std::string type, std::unique_ptr<Agent> agent,
                     const AgentOptions& options = {});

    void runUntil(Timestamp time);
    [[nodiscard]] Simulation& simulation() noexcept;
    [[nodiscard]] const Simulation& simulation() const noexcept;
    // The results so far, with positions valued at the last trade price and the fundamental
    // value as of now.
    [[nodiscard]] RunResult result();

private:
    struct State;
    std::unique_ptr<State> state_;
};

// The exchange's settings for the scenario: those of its [exchange] section, with the reference
// price and the halts of its trading day.
[[nodiscard]] ExchangeConfig exchangeConfig(const Scenario& scenario);

// Builds the scenario's market and runs it for its duration. Every request and event also goes
// to `sink`, if there is one. Throws ScenarioError for settings no market can be built from,
// naming the agent group at fault.
[[nodiscard]] RunResult runScenario(const Scenario& scenario, const AgentRegistry& registry,
                                    EventSink* sink = nullptr);

// Writes a run's settings and results as one JSON object, for analysis scripts. Each group lists
// its agents' ids, which the event log's `agent` column refers to.
void writeResultJson(std::ostream& out, const Scenario& scenario, const RunResult& result);

} // namespace crowdbook

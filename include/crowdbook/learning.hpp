#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/event_log.hpp"
#include "crowdbook/learning_config.hpp"
#include "crowdbook/scenario.hpp"
#include "crowdbook/session.hpp"

namespace crowdbook {

// The name of the learning seat, which joins after the scenario's agents with its [participant]
// account and latency, as a session's seat does.
inline constexpr std::string_view kLearningSeat = "learner";

// The rows of the observation's event stream, by what they report. Each row is
// [type, time, a, b, c, d, e]: the type, the time the seat heard it, and five fields.
enum class LearningEvent : std::int64_t {
    None = 0,       // an empty row
    Trade = 1,      // a: price, b: quantity, c: aggressor, +1 buy and -1 sell, d: 1 in an auction
    Quote = 2,      // the best prices: a, b: bid and its size, c, d: offer and its size; 0 if none
    Depth = 3,      // a: the update's row in `depth`, or -1 if it was not kept
    Fill = 4,       // the seat's: a: side, b: price, c: quantity, d: liquidity, e: fee units
    Accepted = 5,   // the seat's: a: side, b: price, c: quantity, d: 1 for a market order
    Modified = 6,   // the seat's: a: side, b: price, c: open quantity
    Cancelled = 7,  // the seat's: a: side, b: quantity cancelled, c: why
    Rejected = 8,   // the seat's: a: side, b: request, 0 new, 1 cancel, 2 modify, c: why
    Phase = 9,      // a: the phase, b: the price an auction uncrossed at, or 0
    Indicative = 10, // a: price, b: lots that would trade, c: imbalance; 0 if none
    Feed = 11,      // a: the underlying, in millionths of its units
};

inline constexpr std::size_t kEventColumns = 7;
// An open order's row: side, price, open quantity, acknowledged, cancel asked.
inline constexpr std::size_t kOrderColumns = 5;
// How many events, depth updates and open orders there were, all of them.
inline constexpr std::size_t kCountColumns = 3;

// The seat's state at a decision, in this order.
inline constexpr std::array<std::string_view, 18> kLearningStateNames{
    "time",           // since the start, in nanoseconds
    "time_left",      // until the episode ends
    "position",
    "cash",           // change since the start, in tick-lots
    "fees",           // paid since the start, in fee units, thousandths of a tick-lot
    "open_buy",       // lots the seat has bid, including orders in flight
    "open_sell",
    "bid_price",      // the best bid and offer as the seat has heard them; 0 if none
    "bid_quantity",
    "ask_price",
    "ask_quantity",
    "last_trade",     // as the seat has heard it; 0 if none
    "phase",          // of the trading day: 0 continuous, 1 opening, 2 halt, 3 closing, 4 closed
    "reference_price",
    "max_price",
    "max_position",
    "max_order_quantity",
    "rejections",     // of the seat's requests since the last decision
};

[[nodiscard]] std::string_view toString(LearningEvent event) noexcept;

// Where one market writes its observation: buffers the caller owns, sized by the config.
struct LearningObservation {
    std::span<std::int64_t> state;  // kLearningStateNames.size()
    std::span<std::int64_t> orders; // maxOrders × kOrderColumns
    std::span<std::int64_t> events; // maxEvents × kEventColumns
    std::span<std::int64_t> depth;  // maxDepthUpdates × 2 sides × depthLevels × (price, quantity)
    std::span<std::int64_t> book;   // the latest depth heard: 2 × depthLevels × 2
    std::span<std::int64_t> counts; // kCountColumns
};

// What a decision reports beside the observation.
struct LearningStep {
    double reward = 0.0;     // the change in the score since the last decision, in tick-lots
    bool terminated = false; // the episode is over: its time is up, or the loss limit stopped it
    bool stopped = false;    // the loss limit stopped the seat
    Duration elapsed = 0;    // simulated time since the last decision
    Points score = 0;        // the score as the episode would end now, positions closed
    double value = 0.0;      // the true value now, or NaN without one; never in the observation
};

// A scenario's settings drawn for each episode from its [learning.ranges], each from its own
// random stream, keyed by the settings seed and the setting's path, so that the same seed always
// draws the same settings and adding a range changes no other's draw.
class EpisodeScenarios {
public:
    struct Drawn {
        Scenario scenario;
        std::vector<double> settings; // each range's value; for text choices, its index
    };

    // Parses the scenario. With `check`, builds its market with every range at its low end, at
    // the highest value it draws and at each choice, and draws it with settings seeds 1 to 16, so
    // that a range no market can be built from fails now. Throws ScenarioError naming the range at
    // fault.
    EpisodeScenarios(std::string text, std::string source, const AgentRegistry& registry,
                     bool check = true);
    EpisodeScenarios(EpisodeScenarios&&) noexcept;
    EpisodeScenarios& operator=(EpisodeScenarios&&) noexcept;
    ~EpisodeScenarios();

    // The scenario with settings drawn by `settingsSeed`. Throws ScenarioError for a draw no
    // scenario can be read from.
    [[nodiscard]] Drawn draw(std::uint64_t settingsSeed) const;
    // The text of the scenario so drawn, which reads back to it.
    [[nodiscard]] std::string text(std::uint64_t settingsSeed) const;
    [[nodiscard]] const Scenario& base() const noexcept;
    // Each range's path, in the order of Drawn::settings.
    [[nodiscard]] std::vector<std::string> settingNames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One market stepped by a learning agent through its seat: an episode starts with reset and
// goes on, decision by decision, with step, until it is over. Between decisions the market runs
// on: to the next multiple of the interval, or until one of the seat's own orders fills, which
// the seat hears at once and decides on, or, with the feed's trigger, until the feed moves. Every episode is a session with one seat, and session()
// writes it as one, which replays exactly.
class LearningMarket {
public:
    // `shape` fixes what every episode shares: the actions, the sizes and the observation's
    // capacity. Every request and event of the market also goes to `sink`, if there is one.
    LearningMarket(LearningConfig shape, const AgentRegistry& registry, EventSink* sink = nullptr);
    LearningMarket(LearningMarket&&) noexcept;
    LearningMarket& operator=(LearningMarket&&) noexcept;
    ~LearningMarket();

    // Starts an episode of `scenario`, run with its seed, and runs it to the first decision,
    // whose observation it writes. The scenario's [learning] section sets the episode's timing,
    // feed and valuation, or the shape's do without one; its actions, sizes and capacity must be
    // the shape's. `text`, the scenario's text, is what session() records. Returns the first
    // decision's report, with no reward and the time since the start as elapsed; the first
    // step's reward counts from the start, so that an episode's rewards add up to its score.
    // Throws ScenarioError for a scenario no market can be built from, leaving the episode
    // there was, and std::invalid_argument for buffers of other sizes than the shape gives.
    LearningStep reset(Scenario scenario, const LearningObservation& out,
                       std::optional<std::string> text = std::nullopt);
    // Takes action number `action` at size number `size` now, runs to the next decision and
    // writes its observation. Throws std::invalid_argument for an action or size out of range
    // or buffers of the wrong sizes, and std::logic_error once the episode is over.
    LearningStep step(std::size_t action, std::size_t size, const LearningObservation& out);

    [[nodiscard]] bool started() const noexcept;
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] Timestamp now() const;
    // The episode so far as a session with the one seat `learner`: its scenario's text, its seed,
    // and every request the seat sent. Throws std::logic_error without an episode or its text.
    [[nodiscard]] Session session() const;
    [[nodiscard]] const LearningConfig& shape() const noexcept;

private:
    struct Episode;
    LearningConfig shape_;
    const AgentRegistry* registry_;
    EventSink* sink_;
    std::unique_ptr<Episode> episode_;
};

// Many markets in one object, stepped together on threads of their own. Every observation,
// reward and flag is written into buffers the pool owns, which never move, so a caller can read
// them in place; each step overwrites them. Each market is a LearningMarket on its own, so what
// one does depends only on its seeds and actions, never on the number of threads or of markets.
// Calls to reset, step and session from several threads take turns.
class LearningPool {
public:
    // Builds `markets` markets of the scenario in `text`, stepped on `threads` threads, the calling
    // one among them; 0 uses every core. The scenario's [learning] section sets the actions, the
    // sizes and the observation's capacity, and its ranges are checked as EpisodeScenarios does.
    // Nothing runs until reset. Throws ScenarioError as EpisodeScenarios does.
    LearningPool(std::string text, std::string source, std::size_t markets, std::size_t threads = 0,
                 const AgentRegistry& registry = AgentRegistry::withBuiltIns());
    LearningPool(const LearningPool&) = delete;
    LearningPool& operator=(const LearningPool&) = delete;
    ~LearningPool();

    // Starts a new episode in each market for which `mask` is non-zero, or in every market with an
    // empty mask: market i runs with seed seeds[i] and draws its settings with settingsSeeds[i],
    // or seeds[i] when empty. A non-empty scenarios[i] replaces the pool's scenario for that
    // episode. Every span not empty has one entry a market. Markets left out keep their state.
    // Throws std::invalid_argument for spans of the wrong size, and whatever a reset throws.
    void reset(std::span<const std::uint64_t> seeds,
               std::span<const std::uint64_t> settingsSeeds = {},
               std::span<const std::uint8_t> mask = {},
               std::span<const std::optional<std::string>> scenarios = {});
    // Takes each market's action, two numbers a market, the action and the size, and runs every
    // market to its next decision. Throws std::invalid_argument for actions out of range and
    // std::logic_error if a market's episode is over and it has not been reset. If a market
    // throws, the first such market's exception is rethrown once every market has stepped.
    void step(std::span<const std::int64_t> actions);

    [[nodiscard]] std::size_t markets() const noexcept;
    [[nodiscard]] std::size_t threads() const noexcept;
    [[nodiscard]] const LearningConfig& config() const noexcept;
    [[nodiscard]] std::vector<std::string> settingNames() const;
    // Market `market`'s episode as a session. Throws std::out_of_range for a market that does
    // not exist, and as LearningMarket::session does.
    [[nodiscard]] Session session(std::size_t market) const;

    // The buffers, a market after another.
    [[nodiscard]] std::span<std::int64_t> state() noexcept;
    [[nodiscard]] std::span<std::int64_t> orders() noexcept;
    [[nodiscard]] std::span<std::int64_t> events() noexcept;
    [[nodiscard]] std::span<std::int64_t> depth() noexcept;
    [[nodiscard]] std::span<std::int64_t> book() noexcept;
    [[nodiscard]] std::span<std::int64_t> counts() noexcept;
    [[nodiscard]] std::span<double> rewards() noexcept;
    [[nodiscard]] std::span<std::uint8_t> terminated() noexcept;
    // Always 0: the end is part of the task.
    [[nodiscard]] std::span<std::uint8_t> truncated() noexcept;
    [[nodiscard]] std::span<std::uint8_t> stopped() noexcept;
    [[nodiscard]] std::span<std::int64_t> elapsed() noexcept;
    [[nodiscard]] std::span<std::int64_t> score() noexcept;
    [[nodiscard]] std::span<double> value() noexcept;
    [[nodiscard]] std::span<double> settings() noexcept; // markets × settingNames().size()
    [[nodiscard]] std::span<std::uint64_t> seeds() noexcept;
    [[nodiscard]] std::span<std::uint64_t> settingsSeeds() noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace crowdbook

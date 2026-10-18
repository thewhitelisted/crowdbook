#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "crowdbook/detail/ring.hpp"
#include "crowdbook/random.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

struct FundamentalConfig {
    double initial = 10'000.0;           // ticks; also the level the value reverts to
    double meanReversion = 0.0;          // per second; 0 makes the value a random walk
    double volatility = 1.0;             // ticks per square root of a second
    Duration step = 100 * kMillisecond;  // how often the value changes
    // News: jumps at random times, a Poisson process with this many a second on average, each
    // normally distributed with standard deviation jumpSize ticks. 0 means no news.
    double jumpRate = 0.0;
    double jumpSize = 0.0;
};

// A prediction market's share pays this many cents if the answer is yes, and nothing if it is no,
// so it trades from 1 to one less.
inline constexpr Price kPredictionPayout = 100;

// A game between a home and an away side, on a clock, that a prediction market's question can be
// about: will the home side win? Before kickoff the sides' relative strength drifts as information
// arrives, now and then in a jump of news; during play each side scores at random, at a rate its
// strength sets, and the strength can drift too. The clock stops between periods. The true value
// is the probability that the home side ends ahead, given the score, the playing time left and the
// strength as it is now, with a tie paying `tie` cents a share; when the game ends it is 100, 0
// or `tie`. A market that closes before the game ends stops at the probability then.
// A score in a game that was played: when, as time from the market's opening, which side and what
// it was worth.
struct ScriptedScore {
    Duration at = 0;
    bool home = true;
    int points = 1;

    friend bool operator==(const ScriptedScore&, const ScriptedScore&) = default;
};

// Which result of a game a contract on it pays for: a side's win or a draw; the points of both
// sides together over or under a line; or the home side's lead over or under a line, the home
// side or the away side covering a spread.
enum class GameSide : std::uint8_t { Home, Away, Draw, Over, Under, HomeSpread, AwaySpread };

struct GameConfig {
    // The result the contract pays 100 for: a win for the home side, for the away side, or a
    // draw; the total over or under `line`; or the home side's lead over `line` (HomeSpread) or
    // under it (AwaySpread). A contract on a win pays `tie` on a draw, and one on a line pays
    // `tie` where the result is exactly on it.
    GameSide side = GameSide::Home;
    double line = 0.0;                // points, for totals and spreads
    Duration start = 0;               // kickoff, after the market opens
    Duration length = 5400 * kSecond; // playing time, all periods together
    int periods = 1;                  // of equal length, with a break between each two
    Duration breakLength = 0;         // how long each break stops the clock
    double scores = 2.5;              // scoring events expected in the game, both sides together
    // How much faster they come at the end of the playing time than at its start, the rate
    // changing in a straight line between; 1 for an even pace.
    double lateScoring = 1.0;
    std::vector<int> points{1};       // what a scoring event can be worth
    // How often a scoring event is worth each of `points`, in proportion.
    std::vector<double> pointWeights{1.0};
    double tie = 0.0; // cents a share on a win pays if the game ends level
    // The standard deviation of the strength's change before kickoff, and over the playing time.
    double pregameDrift = 0.0;
    double inPlayDrift = 0.0;
    double news = 0.0;     // pieces of news expected before kickoff
    double newsSize = 0.0; // the standard deviation of each, in strength
    // The scores of a game that was played, in order, instead of scores drawn at random: the value
    // still prices what is to come from the expected scoring, but the score follows the script.
    std::vector<ScriptedScore> script{};

    // When the game ends, as time from the market's opening.
    [[nodiscard]] Duration end() const noexcept {
        return start + length + static_cast<Duration>(periods - 1) * breakLength;
    }

    friend bool operator==(const GameConfig&, const GameConfig&) = default;
};

// A prediction market's question, decided at the end of the run by whether a hidden quantity ends
// above zero. The quantity wanders, and news moves it in jumps; its change over the whole run has
// variance 1. The true value is the probability of yes, in cents.
struct PredictionConfig {
    double probability = 0.5; // of yes at the start, strictly between 0 and 1
    double newsRate = 0.0;    // news a second, on average; 0 means no news
    // The share of the outcome's uncertainty that news carries, at least 0 and below 1; the rest
    // comes from the quantity's steady wandering. 0 without news.
    double newsShare = 0.0;
    Duration step = kSecond; // how often the hidden quantity moves
    // A game instead of a hidden quantity: the question is whether the home side wins, and
    // `probability` is its chance at kickoff. It takes no news rate or share of its own; `step` is
    // how often the game moves.
    std::optional<GameConfig> game{};

    friend bool operator==(const PredictionConfig&, const PredictionConfig&) = default;
};

// The asset's true value over time, which informed agents observe with noise. It follows an
// Ornstein–Uhlenbeck process, or a random walk when meanReversion is 0, updated every `step` with
// the exact discrete-time formulas, plus jumps when there is news. It draws only from its own
// random stream, so the path depends on that stream and the config, never on who reads it or
// when; without news it makes no draws for jumps, so turning news off leaves the path as it was.
//
// For a prediction market the walk is the hidden quantity and the value is the exact probability,
// given the path so far, that it ends above zero: a sum over how much news is still to come of
// Φ(x / √(the variance left)). It starts where that probability is the configured one, and at
// resolution it is 100 or 0. For a game, the walk is the score, home less away, and the value the
// probability the home side wins, worked out from the distribution of the points each side has
// still to score.
class Fundamental {
public:
    // Throws std::invalid_argument for a step that is not positive or a negative volatility, mean
    // reversion, jump rate or jump size.
    Fundamental(const FundamentalConfig& config, Random random);
    // A prediction market's value, resolving at `resolution`. Throws std::invalid_argument for a
    // probability outside (0, 1), a negative news rate, a news share outside [0, 1) or one without
    // news, a step that is not positive, or a resolution that is not after the start; for a game,
    // also for news rates or shares beside it, settings out of range, or a probability no strength
    // reaches.
    Fundamental(const PredictionConfig& config, Duration resolution, Random random);

    // Throw what the constructors throw for the same settings, without building the value: a
    // prediction market's starts by searching for where its probability is the one asked for.
    static void check(const FundamentalConfig& config);
    static void check(const PredictionConfig& config, Duration resolution);

    // The value at `time`. Reads may go back in time only as far as `remember` allows, which
    // inside a simulation, whose clock never runs backwards, covers a trader that sees the value
    // late; throws std::invalid_argument for a read further back.
    [[nodiscard]] double valueAt(Timestamp time);
    // Keeps the value for at least `window` before the latest read, for readers that see it late.
    // The longest window asked for wins. Keeping it changes nothing about the path.
    void remember(Duration window);
    // How many jumps the value has taken so far.
    [[nodiscard]] std::int64_t jumps() const noexcept { return jumps_; }
    // Whether the value is a game's.
    [[nodiscard]] bool isGame() const noexcept { return game_.has_value(); }
    // When a prediction market resolves; nullopt for any other value.
    [[nodiscard]] std::optional<Duration> resolution() const noexcept { return resolution_; }
    // The walk behind the value as of the latest read: the value itself, or for a prediction
    // market the hidden quantity that decides it, whose change over the whole run has variance 1,
    // and once resolved, where it ended; for a game, the score, home less away. Reading it moves
    // nothing.
    [[nodiscard]] double walk() const noexcept { return outcome_ ? resolvedWalk_ : value_; }

private:
    // Moves the walk `x` on by one step, with this standard deviation and expected number of
    // jumps.
    void move(double& x, double shockScale, double jumpsExpected);
    // Moves the walk to step `target`, keeping the steps it leaves while they may be read.
    void advanceTo(std::int64_t target);
    // A prediction market's value at step `step`, where the walk is `x`.
    [[nodiscard]] double probabilityAt(std::int64_t step, double x) const noexcept;
    // The probability, in cents, that the hidden quantity ends above zero from `x` with `seconds`
    // to go.
    [[nodiscard]] double probabilityOfYes(double x, double seconds) const noexcept;
    // A game: moved on from one time to another, the playing time by a time, the probability
    // the home side wins with `lead` points and `strength` at `time`, and the distribution of one
    // side's points to come.
    void moveGame(Timestamp from, Timestamp to);
    [[nodiscard]] Duration playedBy(Timestamp time) const noexcept;
    [[nodiscard]] double gameProbability(Timestamp time, double lead, double points,
                                         double strength) const;
    void pointsToCome(double expectedEvents, std::vector<double>& pmf) const;
    // Scoring events expected between two playing times.
    [[nodiscard]] double expectedScores(Duration from, Duration to) const noexcept;
    std::int64_t poisson(double mean);

    FundamentalConfig config_;
    Random random_;
    std::int64_t step_ = 0; // index of the step the current value belongs to
    double value_ = 0.0;
    double decay_ = 1.0;      // how much of the gap to the mean survives one step
    double shockScale_ = 0.0; // standard deviation of the change in one step
    double jumpsPerStep_ = 0.0;
    std::int64_t jumps_ = 0;
    // For a prediction market: when it resolves, the hidden quantity's variance per second
    // between news, each piece of news's variance, the value at the current step and the outcome.
    std::optional<Duration> resolution_{};
    double variancePerSecond_ = 0.0;
    double newsVariance_ = 0.0;
    std::int64_t valuedStep_ = -1;
    double probability_ = 0.0;
    std::optional<double> outcome_{};
    double resolvedWalk_ = 0.0;
    // For a game: its settings, the sides' relative strength now, and work space for the
    // distributions of points to come.
    std::optional<GameConfig> game_{};
    double strength_ = 0.0;
    double points_ = 0.0;          // scored so far, both sides together
    double scoreScale_ = 1.0;      // the expected scoring, as a share of `scores`, for a total
    std::size_t nextScripted_ = 0; // the first scripted score not yet played
    mutable std::vector<double> homePoints_;
    mutable std::vector<double> awayPoints_;
    // A step before the current one: the walk then, and for a prediction market its value, worked
    // out the first time it is read, so that readers late by different delays work out each
    // step's probability once between them.
    struct RememberedStep {
        double walk = 0.0;
        double value = 0.0;
        double strength = 0.0; // a game's
        double points = 0.0;
    };
    // The steps before the current one, as far back as readers may look, oldest first.
    std::int64_t remembered_ = 0; // how many steps to keep
    detail::Ring<RememberedStep> history_;
};

} // namespace crowdbook

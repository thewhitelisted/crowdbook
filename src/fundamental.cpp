#include "crowdbook/fundamental.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

#include "crowdbook/math.hpp"

namespace crowdbook {

Fundamental::Fundamental(const FundamentalConfig& config, Random random)
    : config_(config), random_(random), value_(config.initial) {
    check(config);
    const double seconds = static_cast<double>(config.step) / static_cast<double>(kSecond);
    jumpsPerStep_ = config.jumpRate * seconds;
    const double theta = config.meanReversion;
    if (theta > 0.0) {
        decay_ = math::exp(-theta * seconds);
        shockScale_ = config.volatility * std::sqrt((1.0 - decay_ * decay_) / (2.0 * theta));
    } else {
        decay_ = 1.0;
        shockScale_ = config.volatility * std::sqrt(seconds);
    }
}

void Fundamental::check(const FundamentalConfig& config) {
    if (config.step <= 0) {
        throw std::invalid_argument("the fundamental's step must be positive");
    }
    for (const double setting : {config.initial, config.volatility, config.meanReversion,
                                 config.jumpRate, config.jumpSize}) {
        if (!std::isfinite(setting)) {
            throw std::invalid_argument("the fundamental's settings must be finite numbers");
        }
    }
    if (!(config.volatility >= 0.0) || !(config.meanReversion >= 0.0)) {
        throw std::invalid_argument(
            "the fundamental's volatility and mean reversion must not be negative");
    }
    if (!(config.jumpRate >= 0.0) || !(config.jumpSize >= 0.0)) {
        throw std::invalid_argument("the jump rate and jump size must not be negative");
    }
}

namespace {

// A remembered step whose value no reader has asked for yet.
constexpr double kNotWorkedOut = std::numeric_limits<double>::quiet_NaN();

void checkGame(const GameConfig& game, const PredictionConfig& config) {
    if (config.newsRate != 0.0 || config.newsShare != 0.0) {
        throw std::invalid_argument(
            "a game brings its own news: give it news and news_size, not a news rate or share");
    }
    if (game.start < 0 || game.length <= 0 || game.breakLength < 0 || game.periods < 1 ||
        game.periods > 100) {
        throw std::invalid_argument("a game needs a kickoff that is not negative, a positive "
                                    "length, from 1 to 100 periods and breaks that are not "
                                    "negative");
    }
    if (!(game.scores > 0.0 && game.scores <= 500.0)) {
        throw std::invalid_argument("a game expects more than 0 and at most 500 scores");
    }
    if (game.points.empty() || game.points.size() != game.pointWeights.size()) {
        throw std::invalid_argument("a game needs as many weights as values of a score");
    }
    for (std::size_t i = 0; i < game.points.size(); ++i) {
        if (game.points[i] < 1 || game.points[i] > 100 || !(game.pointWeights[i] > 0.0) ||
            !std::isfinite(game.pointWeights[i])) {
            throw std::invalid_argument(
                "a score is worth from 1 to 100 points, each with a positive weight");
        }
    }
    if (!(game.lateScoring > 0.0 && game.lateScoring <= 100.0)) {
        throw std::invalid_argument("late scoring must be above 0 and at most 100");
    }
    if (!(game.tie >= 0.0 && game.tie <= 100.0)) {
        throw std::invalid_argument("a tie pays from 0 to 100 cents");
    }
    for (const double setting : {game.pregameDrift, game.inPlayDrift, game.news, game.newsSize}) {
        if (!(setting >= 0.0) || !std::isfinite(setting)) {
            throw std::invalid_argument("a game's drifts and news must not be negative");
        }
    }
    if (game.news > 500.0) {
        throw std::invalid_argument("at most 500 pieces of news are expected before kickoff");
    }
    Duration last = 0;
    for (const ScriptedScore& score : game.script) {
        if (score.at < last || score.points < 1 || score.points > 100) {
            throw std::invalid_argument("a game's scripted scores must come in order, at times "
                                        "that are not negative, each worth 1 to 100 points");
        }
        last = score.at;
    }
}

// What the walk of a prediction market is: no pull towards a mean, and the given variances.
FundamentalConfig walkOf(const PredictionConfig& config, Duration resolution) {
    if (!(config.probability > 0.0 && config.probability < 1.0)) {
        throw std::invalid_argument("the probability must be between 0 and 1");
    }
    if (!(config.newsRate >= 0.0) || !std::isfinite(config.newsRate)) {
        throw std::invalid_argument("the news rate must not be negative");
    }
    if (!(config.newsShare >= 0.0 && config.newsShare < 1.0)) {
        throw std::invalid_argument("the news share must be at least 0 and below 1");
    }
    if (config.newsShare > 0.0 && config.newsRate == 0.0) {
        throw std::invalid_argument("a news share needs news: give a news rate");
    }
    if (config.step <= 0 || resolution <= 0) {
        throw std::invalid_argument("the step and the time to resolution must be positive");
    }
    if (const auto& game = config.game) {
        checkGame(*game, config);
        return {.initial = 0.0, .meanReversion = 0.0, .volatility = 0.0, .step = config.step};
    }
    const double seconds = static_cast<double>(resolution) / static_cast<double>(kSecond);
    if (config.newsRate * seconds > 500.0) {
        throw std::invalid_argument(
            "at most 500 pieces of news are expected before resolution; lower the news rate");
    }
    return {.initial = 0.0,
            .meanReversion = 0.0,
            .volatility = std::sqrt((1.0 - config.newsShare) / seconds),
            .step = config.step,
            .jumpRate = config.newsRate,
            .jumpSize = config.newsRate > 0.0
                            ? std::sqrt(config.newsShare / (config.newsRate * seconds))
                            : 0.0};
}

} // namespace

void Fundamental::check(const PredictionConfig& config, Duration resolution) {
    check(walkOf(config, resolution));
    if (config.game) {
        // Whether some strength gives the probability asked for takes working the game out.
        const Fundamental probe{config, resolution, Random{0, 0}};
    }
}

Fundamental::Fundamental(const PredictionConfig& config, Duration resolution, Random random)
    : Fundamental(walkOf(config, resolution), random) {
    resolution_ = resolution;
    if (config.game) {
        // Start the strength where the home side's chance at kickoff is the one asked for. It
        // rises with the strength, so bisection finds it.
        game_ = config.game;
        // A home win's chance rises with the home side's strength and an away win's falls; a
        // draw's falls as either side grows stronger, so for a draw the home side is taken to be
        // the stronger, its strength not negative.
        const double sign = game_->side == GameSide::Home ? 1.0 : -1.0;
        const auto rising = [&](double strength) {
            return sign * gameProbability(0, 0.0, strength);
        };
        const double target = sign * 100.0 * config.probability;
        double low = game_->side == GameSide::Draw ? 0.0 : -30.0;
        double high = 30.0;
        if (rising(low) > target || rising(high) < target) {
            throw std::invalid_argument(std::format(
                "no strength gives the contract a price of {} at kickoff with {} scores expected",
                config.probability, game_->scores));
        }
        for (int i = 0; i < 200; ++i) {
            const double middle = (low + high) / 2.0;
            if (middle == low || middle == high) {
                break;
            }
            (rising(middle) < target ? low : high) = middle;
        }
        strength_ = (low + high) / 2.0;
        value_ = 0.0;
        return;
    }
    variancePerSecond_ = config_.volatility * config_.volatility;
    newsVariance_ = config_.jumpSize * config_.jumpSize;
    // Start where the probability of yes is the one asked for. It rises with the starting point,
    // so bisection finds it, the same way everywhere.
    const double seconds = static_cast<double>(resolution) / static_cast<double>(kSecond);
    const double target = 100.0 * config.probability;
    double low = -40.0;
    double high = 40.0;
    for (int i = 0; i < 200 && low < high; ++i) {
        const double middle = (low + high) / 2.0;
        if (middle == low || middle == high) {
            break;
        }
        (probabilityOfYes(middle, seconds) < target ? low : high) = middle;
    }
    value_ = (low + high) / 2.0;
}

void Fundamental::move(double& x, double shockScale, double jumpsExpected) {
    x = config_.initial + (x - config_.initial) * decay_ + shockScale * random_.normal(0.0, 1.0);
    if (jumpsExpected > 0.0) {
        // The number of jumps in one step is Poisson: walk its distribution to a uniform draw.
        const double draw = random_.uniform();
        double probability = math::exp(-jumpsExpected);
        double cumulative = probability;
        std::int64_t count = 0;
        while (draw > cumulative && probability > 0.0) {
            ++count;
            probability *= jumpsExpected / static_cast<double>(count);
            cumulative += probability;
        }
        for (std::int64_t jump = 0; jump < count; ++jump) {
            x += random_.normal(0.0, config_.jumpSize);
        }
        jumps_ += count;
    }
}

void Fundamental::advanceTo(std::int64_t target) {
    for (; step_ < target; ++step_) {
        if (remembered_ > 0) {
            history_.pushBack({.walk = value_,
                               .value = valuedStep_ == step_ ? probability_ : kNotWorkedOut,
                               .strength = strength_});
            if (std::cmp_greater(history_.size(), remembered_)) {
                history_.popFront();
            }
        }
        if (game_) {
            moveGame(step_ * config_.step, (step_ + 1) * config_.step);
        } else {
            move(value_, shockScale_, jumpsPerStep_);
        }
    }
}

std::int64_t Fundamental::poisson(double mean) {
    // Walk the distribution to a uniform draw.
    const double draw = random_.uniform();
    double probability = math::exp(-mean);
    double cumulative = probability;
    std::int64_t count = 0;
    while (draw > cumulative && probability > 0.0) {
        ++count;
        probability *= mean / static_cast<double>(count);
        cumulative += probability;
    }
    return count;
}

Duration Fundamental::playedBy(Timestamp time) const noexcept {
    const GameConfig& game = *game_;
    if (time <= game.start) {
        return 0;
    }
    const Duration since = time - game.start;
    const Duration period = game.length / game.periods;
    const Duration cycle = period + game.breakLength;
    const std::int64_t index = since / cycle;
    if (index >= game.periods - 1) {
        // The last period takes whatever the others leave of the length.
        const std::int64_t before = game.periods - 1;
        return std::min(before * period + std::max<Duration>(since - before * cycle, 0),
                        game.length);
    }
    return index * period + std::min(since - index * cycle, period);
}

void Fundamental::moveGame(Timestamp from, Timestamp to) {
    const GameConfig& game = *game_;
    const auto share = [](Duration part, Duration whole) {
        return static_cast<double>(part) / static_cast<double>(whole);
    };
    if (from < game.start) {
        const double before = share(std::min(to, game.start) - from, game.start);
        if (game.pregameDrift > 0.0) {
            strength_ += game.pregameDrift * std::sqrt(before) * random_.normal(0.0, 1.0);
        }
        if (game.news > 0.0) {
            const std::int64_t pieces = poisson(game.news * before);
            for (std::int64_t piece = 0; piece < pieces; ++piece) {
                strength_ += random_.normal(0.0, game.newsSize);
            }
            jumps_ += pieces;
        }
    }
    // The game that was played, whatever the strength says, at the times it was played, even
    // where they fall outside the clock, as in time added on.
    for (; nextScripted_ < game.script.size() && game.script[nextScripted_].at <= to;
         ++nextScripted_) {
        const ScriptedScore& score = game.script[nextScripted_];
        value_ += score.home ? score.points : -score.points;
        ++jumps_;
    }
    const Duration played = playedBy(to) - playedBy(from);
    if (played <= 0) {
        return;
    }
    const double part = share(played, game.length);
    if (game.inPlayDrift > 0.0) {
        strength_ += game.inPlayDrift * std::sqrt(part) * random_.normal(0.0, 1.0);
    }
    const double homeShare = 1.0 / (1.0 + math::exp(-strength_));
    double total = 0.0;
    for (const double weight : game.pointWeights) {
        total += weight;
    }
    const auto pointsOf = [&](std::int64_t events) {
        std::int64_t points = 0;
        for (std::int64_t event = 0; event < events; ++event) {
            double draw = random_.uniform() * total;
            std::size_t which = 0;
            while (which + 1 < game.points.size() && draw >= game.pointWeights[which]) {
                draw -= game.pointWeights[which];
                ++which;
            }
            points += game.points[which];
        }
        return points;
    };
    if (!game.script.empty()) {
        return;
    }
    const double expected = expectedScores(playedBy(from), playedBy(to));
    const std::int64_t home = poisson(expected * homeShare);
    const std::int64_t away = poisson(expected * (1.0 - homeShare));
    value_ += static_cast<double>(pointsOf(home) - pointsOf(away));
    jumps_ += home + away;
}

void Fundamental::pointsToCome(double expectedEvents, std::vector<double>& pmf) const {
    // A compound Poisson distribution, by Panjer's recursion: g(0) = e^−λ and
    // g(k) = λ/k · Σ_j j·w_j·g(k − j) over the values j a score can be worth, with weights w_j
    // that sum to 1.
    const GameConfig& game = *game_;
    double total = 0.0;
    double mean = 0.0;
    double square = 0.0;
    int largest = 0;
    for (std::size_t i = 0; i < game.points.size(); ++i) {
        total += game.pointWeights[i];
    }
    for (std::size_t i = 0; i < game.points.size(); ++i) {
        const double p = game.points[i];
        mean += game.pointWeights[i] / total * p;
        square += game.pointWeights[i] / total * p * p;
        largest = std::max(largest, game.points[i]);
    }
    const double reach =
        expectedEvents * mean + 12.0 * std::sqrt(expectedEvents * square) + 3.0 * largest;
    pmf.assign(static_cast<std::size_t>(std::ceil(reach)) + 1, 0.0);
    pmf[0] = math::exp(-expectedEvents);
    for (std::size_t k = 1; k < pmf.size(); ++k) {
        double sum = 0.0;
        for (std::size_t i = 0; i < game.points.size(); ++i) {
            const auto j = static_cast<std::size_t>(game.points[i]);
            if (j <= k) {
                sum += static_cast<double>(j) * game.pointWeights[i] / total * pmf[k - j];
            }
        }
        pmf[k] = expectedEvents / static_cast<double>(k) * sum;
    }
}

double Fundamental::expectedScores(Duration from, Duration to) const noexcept {
    // The rate runs in a straight line through the playing time, from a at its start to
    // a · late_scoring at its end, with a set so that the whole game expects `scores`.
    const GameConfig& game = *game_;
    const double u0 = static_cast<double>(from) / static_cast<double>(game.length);
    const double u1 = static_cast<double>(to) / static_cast<double>(game.length);
    const double start = 2.0 / (1.0 + game.lateScoring);
    const double slope = start * (game.lateScoring - 1.0);
    return game.scores * (start * (u1 - u0) + slope / 2.0 * (u1 * u1 - u0 * u0));
}

double Fundamental::gameProbability(Timestamp time, double lead, double strength) const {
    const GameConfig& game = *game_;
    // What the contract pays on each result.
    const auto pays = [&](double win, double tie, double loss) {
        switch (game.side) {
        case GameSide::Home:
            return 100.0 * win + game.tie * tie;
        case GameSide::Away:
            return 100.0 * loss + game.tie * tie;
        case GameSide::Draw:
            return 100.0 * tie;
        }
        return 0.0;
    };
    const Duration left = game.length - playedBy(time);
    if (left <= 0) {
        return pays(lead > 0.0 ? 1.0 : 0.0, lead == 0.0 ? 1.0 : 0.0, lead < 0.0 ? 1.0 : 0.0);
    }
    const double events = expectedScores(game.length - left, game.length);
    const double homeShare = 1.0 / (1.0 + math::exp(-strength));
    pointsToCome(events * homeShare, homePoints_);
    pointsToCome(events * (1.0 - homeShare), awayPoints_);
    // The home side's distribution becomes its tail: tail[k], the chance of k points or more, so
    // that it ends ahead with more than the away side's points less its lead, and level with
    // exactly that. Both distributions are cut off far out, so each is divided by its mass.
    std::vector<double>& tail = homePoints_;
    tail.push_back(0.0);
    for (std::size_t k = tail.size() - 1; k-- > 0;) {
        tail[k] += tail[k + 1];
    }
    const double homeMass = tail[0];
    const auto ahead = static_cast<std::int64_t>(std::llround(lead));
    double win = 0.0;
    double tie = 0.0;
    double awayMass = 0.0;
    for (std::size_t a = 0; a < awayPoints_.size(); ++a) {
        awayMass += awayPoints_[a];
        const std::int64_t level = static_cast<std::int64_t>(a) - ahead;
        if (level < 0) {
            win += awayPoints_[a] * homeMass;
        } else if (std::cmp_less(level + 1, tail.size())) {
            const auto k = static_cast<std::size_t>(level);
            win += awayPoints_[a] * tail[k + 1];
            tie += awayPoints_[a] * (tail[k] - tail[k + 1]);
        }
    }
    const double mass = awayMass * homeMass;
    win /= mass;
    tie /= mass;
    return pays(win, tie, std::max(1.0 - win - tie, 0.0));
}

void Fundamental::remember(Duration window) {
    if (window > 0) {
        remembered_ = std::max(remembered_, window / config_.step + 1);
    }
}

double Fundamental::probabilityAt(std::int64_t step, double x) const noexcept {
    const Duration left = *resolution_ - step * config_.step;
    return probabilityOfYes(x, static_cast<double>(left) / static_cast<double>(kSecond));
}

double Fundamental::probabilityOfYes(double x, double seconds) const noexcept {
    // The change still to come is normal with variance variancePerSecond_ * seconds, plus a
    // Poisson number of pieces of news, each normal with variance newsVariance_: sum over the
    // number of pieces, until the rest of the Poisson weights is negligible.
    const double steady = variancePerSecond_ * seconds;
    const double expected = config_.jumpRate * seconds;
    double weight = math::exp(-expected);
    double sum = 0.0;
    for (int pieces = 0;; ++pieces) {
        if (pieces > 0) {
            weight *= expected / static_cast<double>(pieces);
        }
        // Terms this unlikely cannot change a probability in cents, so they are not worked out.
        if (weight > 1e-20) {
            const double variance = steady + static_cast<double>(pieces) * newsVariance_;
            const double cdf = variance > 0.0 ? math::normalCdf(x / std::sqrt(variance))
                                              : (x > 0.0 ? 1.0 : 0.0);
            sum += weight * cdf;
        }
        // Past the most likely number of pieces the weights only shrink.
        if (static_cast<double>(pieces) >= expected && weight < 1e-18) {
            break;
        }
    }
    return 100.0 * sum;
}

double Fundamental::valueAt(Timestamp time) {
    if (resolution_ && time >= *resolution_) {
        if (!outcome_) {
            // Every whole step up to resolution, then whatever is left of the last one, which
            // decides the question but is no step anyone can read.
            const std::int64_t last = *resolution_ / config_.step;
            if (last < step_) {
                throw std::invalid_argument("the fundamental cannot go back in time");
            }
            advanceTo(last);
            resolvedWalk_ = value_;
            const Duration rest = *resolution_ - last * config_.step;
            if (game_) {
                if (rest > 0) {
                    moveGame(last * config_.step, *resolution_);
                }
                resolvedWalk_ = value_;
                // A market that closes before the final whistle stops at its value then.
                outcome_ = gameProbability(*resolution_, value_, strength_);
                return *outcome_;
            }
            if (rest > 0) {
                const double seconds = static_cast<double>(rest) / static_cast<double>(kSecond);
                move(resolvedWalk_, config_.volatility * std::sqrt(seconds),
                     config_.jumpRate * seconds);
            }
            outcome_ = resolvedWalk_ > 0.0 ? 100.0 : 0.0;
        }
        return *outcome_;
    }
    const std::int64_t target = time < 0 ? 0 : time / config_.step;
    if (target < step_) {
        // A reader that sees the value late, within what is remembered.
        const std::int64_t back = step_ - target;
        if (std::cmp_greater(back, history_.size())) {
            throw std::invalid_argument(std::format(
                "the fundamental was read at step {} after step {}, further back than it "
                "remembers",
                target, step_));
        }
        RememberedStep& then = history_[history_.size() - static_cast<std::size_t>(back)];
        if (!resolution_) {
            return then.walk;
        }
        if (std::isnan(then.value)) {
            then.value = game_ ? gameProbability(target * config_.step, then.walk, then.strength)
                               : probabilityAt(target, then.walk);
        }
        return then.value;
    }
    advanceTo(target);
    if (!resolution_) {
        return value_;
    }
    // The probability changes only when the walk moves: work it out once a step.
    if (valuedStep_ != step_) {
        probability_ = game_ ? gameProbability(step_ * config_.step, value_, strength_)
                             : probabilityAt(step_, value_);
        valuedStep_ = step_;
    }
    return probability_;
}

} // namespace crowdbook

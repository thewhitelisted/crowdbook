#include "crowdbook/fundamental.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

#include "crowdbook/math.hpp"

namespace crowdbook {

Fundamental::Fundamental(const FundamentalConfig& config, Random random)
    : config_(config), random_(random), value_(config.initial) {
    check(config);
    if (!config.path.empty()) {
        recorded_ = true;
        path_ = config.path;
        pathStart_ = config.initial;
        pathOutcome_ = path_.back().value;
    }
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
    if (!config.path.empty()) {
        if (config.meanReversion != 0.0 || config.volatility != 0.0 || config.jumpRate != 0.0 ||
            config.jumpSize != 0.0) {
            throw std::invalid_argument(
                "a recorded value path takes no mean reversion, volatility or jumps");
        }
        Duration last = 0;
        for (const ValuePoint& point : config.path) {
            if (point.at < last || !std::isfinite(point.value)) {
                throw std::invalid_argument("a value path's points must come in order, at times "
                                            "that are not negative, with finite values");
            }
            last = point.at;
        }
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
    const bool lined = game.side == GameSide::Over || game.side == GameSide::Under ||
                       game.side == GameSide::HomeSpread || game.side == GameSide::AwaySpread;
    if (!lined && game.line != 0.0) {
        throw std::invalid_argument("a line goes with a total or a spread, not a win or a draw");
    }
    if (!std::isfinite(game.line)) {
        throw std::invalid_argument("a game's line must be a number");
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
    if (!config.path.empty() || config.outcome) {
        if (config.game || config.newsRate != 0.0 || config.newsShare != 0.0) {
            throw std::invalid_argument(
                "a recorded value path takes no game and no news rate or share");
        }
        Duration last = 0;
        for (const ValuePoint& point : config.path) {
            if (point.at < last || !(point.value >= 0.0 && point.value <= 100.0)) {
                throw std::invalid_argument("a value path's points must come in order, at times "
                                            "that are not negative, with values from 0 to 100");
            }
            last = point.at;
        }
        if (config.outcome && !(*config.outcome >= 0.0 && *config.outcome <= 100.0)) {
            throw std::invalid_argument("a value path's outcome is from 0 to 100");
        }
        return {.initial = 0.0, .meanReversion = 0.0, .volatility = 0.0, .step = config.step};
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
    if (!config.path.empty() || config.outcome) {
        recorded_ = true;
        path_ = config.path;
        pathStart_ = 100.0 * config.probability;
        pathOutcome_ = config.outcome.value_or(path_.empty() ? pathStart_ : path_.back().value);
        value_ = pathStart_;
        return;
    }
    if (config.game) {
        // Start the strength where the home side's chance at kickoff is the one asked for. It
        // rises with the strength, so bisection finds it.
        game_ = config.game;
        // A home win's chance rises with the home side's strength and an away win's falls; a
        // draw's falls as either side grows stronger, so for a draw the home side is taken to be
        // the stronger, its strength not negative.
        // For a total the strength does not share the scoring out, which a total does not see,
        // but scales how much is scored, by e^strength: the drift and the news before kickoff
        // move what is expected. It starts at 0, and the price at kickoff sets how much is
        // expected to be scored instead, in proportion to `scores`, a total over the line rising
        // with it.
        const GameSide side = game_->side;
        const bool total = side == GameSide::Over || side == GameSide::Under;
        const double sign =
            side == GameSide::Home || side == GameSide::HomeSpread || side == GameSide::Over ? 1.0
                                                                                             : -1.0;
        const auto rising = [&](double setting) {
            if (total) {
                scoreScale_ = std::exp(setting);
                return sign * gameProbability(0, 0.0, 0.0, 0.0);
            }
            return sign * gameProbability(0, 0.0, 0.0, setting);
        };
        const double target = sign * 100.0 * config.probability;
        double low = side == GameSide::Draw ? 0.0 : (total ? -8.0 : -30.0);
        double high = total ? 5.0 : 30.0;
        // Written to fail on a price that is not a number too, as one is where so much is
        // expected to be scored that the chance of none underflows.
        if (!(rising(low) <= target && rising(high) >= target)) {
            throw std::invalid_argument(std::format(
                "no {} gives the contract a price of {} at kickoff with {} scores expected",
                total ? "scoring" : "strength", config.probability, game_->scores));
        }
        for (int i = 0; i < 200; ++i) {
            const double middle = (low + high) / 2.0;
            if (middle == low || middle == high) {
                break;
            }
            (rising(middle) < target ? low : high) = middle;
        }
        if (total) {
            scoreScale_ = std::exp((low + high) / 2.0);
        } else {
            strength_ = (low + high) / 2.0;
        }
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
                               .strength = strength_,
                               .points = points_});
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
    // For a total the strength scales the scoring by e^strength, so each move gives up half its
    // variance, keeping the scoring expected where it was: e^strength stays a fair multiplier.
    const bool onTotal = game.side == GameSide::Over || game.side == GameSide::Under;
    const auto drift = [&](double sd) {
        strength_ += sd * random_.normal(0.0, 1.0) - (onTotal ? sd * sd / 2.0 : 0.0);
    };
    if (from < game.start) {
        const double before = share(std::min(to, game.start) - from, game.start);
        if (game.pregameDrift > 0.0) {
            drift(game.pregameDrift * std::sqrt(before));
        }
        if (game.news > 0.0) {
            const std::int64_t pieces = poisson(game.news * before);
            for (std::int64_t piece = 0; piece < pieces; ++piece) {
                drift(game.newsSize);
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
        points_ += score.points;
        ++jumps_;
    }
    const Duration played = playedBy(to) - playedBy(from);
    if (played <= 0) {
        return;
    }
    const double part = share(played, game.length);
    if (game.inPlayDrift > 0.0) {
        drift(game.inPlayDrift * std::sqrt(part));
    }
    const double homeShare = onTotal ? 0.5 : 1.0 / (1.0 + math::exp(-strength_));
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
    const double expected =
        expectedScores(playedBy(from), playedBy(to)) * (onTotal ? math::exp(strength_) : 1.0);
    const std::int64_t home = poisson(expected * homeShare);
    const std::int64_t away = poisson(expected * (1.0 - homeShare));
    const std::int64_t homePoints = pointsOf(home);
    const std::int64_t awayPoints = pointsOf(away);
    value_ += static_cast<double>(homePoints - awayPoints);
    points_ += static_cast<double>(homePoints + awayPoints);
    jumps_ += home + away;
}

namespace {
// Above this many scoring events expected, e^−λ comes near the smallest double and points to come
// are taken as normal.
constexpr double kNormalEvents = 600.0;
} // namespace

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
    if (expectedEvents > kNormalEvents) {
        // So many events that e^−λ underflows: the count is normal to a very good
        // approximation, with mean λ·E[j] and variance λ·E[j²], spread over whole points.
        const double centre = expectedEvents * mean;
        const double sd = std::sqrt(expectedEvents * square);
        for (std::size_t k = 0; k < pmf.size(); ++k) {
            const auto at = static_cast<double>(k);
            pmf[k] = math::normalCdf((at + 0.5 - centre) / sd) -
                     math::normalCdf((at - 0.5 - centre) / sd);
        }
        return;
    }
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
    return scoreScale_ * game.scores * (start * (u1 - u0) + slope / 2.0 * (u1 * u1 - u0 * u0));
}

double Fundamental::strengthVarianceLeft(Timestamp time) const noexcept {
    const GameConfig& game = *game_;
    double variance = 0.0;
    if (time < game.start) {
        const double before =
            static_cast<double>(game.start - time) / static_cast<double>(game.start);
        variance +=
            (game.pregameDrift * game.pregameDrift + game.news * game.newsSize * game.newsSize) *
            before;
    }
    const Duration left = game.length - playedBy(time);
    if (left > 0) {
        variance += game.inPlayDrift * game.inPlayDrift * static_cast<double>(left) /
                    static_cast<double>(game.length);
    }
    return variance;
}

double Fundamental::gameProbability(Timestamp time, double lead, double points,
                                    double strength) const {
    // The strength still moves before the game ends, and the price is not a straight line in
    // it, so the price is its average over where the strength may go: normal, with the variance
    // left, and for a total, whose scoring scales by e^strength, centred so that e^strength
    // keeps its mean. Gauss–Hermite quadrature with nine points does the average.
    static constexpr std::array<double, 9> kNodes{
        -4.512745863399783, -3.20542900285647, -2.07684797867783, -1.0232556637891326, 0.0,
        1.0232556637891326, 2.07684797867783,  3.20542900285647,  4.512745863399783};
    static constexpr std::array<double, 9> kWeights{
        2.2345844007746607e-05, 0.0027891413212317692, 0.04991640676521782,
        0.24409750289493953,    0.40634920634920635,   0.24409750289493953,
        0.04991640676521782,    0.0027891413212317692, 2.2345844007746607e-05};
    const double variance = strengthVarianceLeft(time);
    if (variance <= 0.0) {
        return probabilityAtStrength(time, lead, points, strength);
    }
    const bool total = game_->side == GameSide::Over || game_->side == GameSide::Under;
    const double sd = std::sqrt(variance);
    const double centre = strength - (total ? variance / 2.0 : 0.0);
    double price = 0.0;
    for (std::size_t i = 0; i < kNodes.size(); ++i) {
        price += kWeights[i] * probabilityAtStrength(time, lead, points, centre + sd * kNodes[i]);
    }
    return price;
}

double Fundamental::probabilityAtStrength(Timestamp time, double lead, double points,
                                          double strength) const {
    const GameConfig& game = *game_;
    // What the contract pays on each result, its chances of being above, on and below the mark
    // it is about: the lead against 0 or the line, or the total against the line.
    const auto pays = [&](double above, double on, double below) {
        switch (game.side) {
        case GameSide::Home:
        case GameSide::Over:
        case GameSide::HomeSpread:
            return 100.0 * above + game.tie * on;
        case GameSide::Away:
        case GameSide::Under:
        case GameSide::AwaySpread:
            return 100.0 * below + game.tie * on;
        case GameSide::Draw:
            return 100.0 * on;
        }
        return 0.0;
    };
    const bool total = game.side == GameSide::Over || game.side == GameSide::Under;
    const bool spread = game.side == GameSide::HomeSpread || game.side == GameSide::AwaySpread;
    const double mark = total || spread ? game.line : 0.0;
    const double now = total ? points : lead;
    const Duration left = game.length - playedBy(time);
    if (left <= 0) {
        return pays(now > mark ? 1.0 : 0.0, now == mark ? 1.0 : 0.0, now < mark ? 1.0 : 0.0);
    }
    const double events = expectedScores(game.length - left, game.length);
    if (total) {
        // Both sides' points together are one compound Poisson count, at both rates together,
        // which ends above the line when more than `needed` points are still to come. The
        // distribution is cut off far out, so it is divided by its mass.
        pointsToCome(events * math::exp(strength), homePoints_);
        const double needed = mark - now;
        double mass = 0.0;
        double above = 0.0;
        double on = 0.0;
        for (std::size_t k = 0; k < homePoints_.size(); ++k) {
            const auto count = static_cast<double>(k);
            mass += homePoints_[k];
            above += count > needed ? homePoints_[k] : 0.0;
            on += count == needed ? homePoints_[k] : 0.0;
        }
        above /= mass;
        on /= mass;
        return pays(above, on, std::max(1.0 - above - on, 0.0));
    }
    const double homeShare = 1.0 / (1.0 + math::exp(-strength));
    pointsToCome(events * homeShare, homePoints_);
    pointsToCome(events * (1.0 - homeShare), awayPoints_);
    // The home side's lead ends above the mark when its points to come exceed the away side's
    // plus the mark less the lead now: summed over the away side's, through the home side's
    // tail, tail[k] the chance of k points or more.
    std::vector<double>& tail = homePoints_;
    tail.push_back(0.0);
    for (std::size_t k = tail.size() - 1; k-- > 0;) {
        tail[k] += tail[k + 1];
    }
    const double homeMass = tail[0];
    const auto atLeast = [&](double count) {
        if (count <= 0.0) {
            return homeMass;
        }
        const auto index = static_cast<std::size_t>(count);
        return index < tail.size() ? tail[index] : 0.0;
    };
    double above = 0.0;
    double on = 0.0;
    double awayMass = 0.0;
    for (std::size_t a = 0; a < awayPoints_.size(); ++a) {
        awayMass += awayPoints_[a];
        // The home side's points to come must exceed this, or equal it for the lead to end on
        // the mark.
        const double needed = static_cast<double>(a) + mark - now;
        if (needed < 0.0) {
            above += awayPoints_[a] * homeMass;
            continue;
        }
        const double fewest = std::floor(needed) + 1.0; // the fewest points that exceed it
        above += awayPoints_[a] * atLeast(fewest);
        if (needed == std::floor(needed)) {
            on += awayPoints_[a] * (atLeast(needed) - atLeast(needed + 1.0));
        }
    }
    const double mass = awayMass * homeMass;
    above /= mass;
    on /= mass;
    return pays(above, on, std::max(1.0 - above - on, 0.0));
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

double Fundamental::pathValue(Timestamp time) const noexcept {
    const auto after = std::ranges::upper_bound(path_, time, {}, &ValuePoint::at);
    return after == path_.begin() ? pathStart_ : std::prev(after)->value;
}

double Fundamental::valueAt(Timestamp time) {
    if (recorded_) {
        // A recorded path is a function of time alone: read at any time, in any order.
        value_ = resolution_ && time >= *resolution_ ? pathOutcome_ : pathValue(time);
        return value_;
    }
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
                outcome_ = gameProbability(*resolution_, value_, points_, strength_);
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
            then.value = game_ ? gameProbability(target * config_.step, then.walk, then.points,
                                                 then.strength)
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
        probability_ = game_ ? gameProbability(step_ * config_.step, value_, points_, strength_)
                             : probabilityAt(step_, value_);
        valuedStep_ = step_;
    }
    return probability_;
}

} // namespace crowdbook

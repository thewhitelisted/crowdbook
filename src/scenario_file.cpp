#include "crowdbook/scenario_file.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <toml++/toml.hpp>

#include "scenario_toml.hpp"

namespace crowdbook {

namespace {

using Keys = std::initializer_list<std::string_view>;

// The largest loss limit: far enough inside int64 that the arithmetic around it cannot overflow.
constexpr Cash kMaxCashLimit = std::int64_t{1} << 60;

// Reads values out of one TOML document, reporting problems as "source:line: message".
class Reader {
public:
    explicit Reader(std::string_view source) noexcept : source_(source) {}

    [[noreturn]] void fail(const toml::node& node, std::string_view message) const {
        throw ScenarioError(std::format("{}:{}: {}", source_, node.source().begin.line, message));
    }

    void allowOnly(const toml::table& table, Keys allowed, std::string_view where) const {
        for (auto&& [key, node] : table) {
            if (std::ranges::find(allowed, key.str()) == allowed.end()) {
                std::string expected;
                for (const std::string_view name : allowed) {
                    expected += expected.empty() ? std::string{name} : std::format(", {}", name);
                }
                fail(node, std::format("unknown key '{}' {}; expected one of: {}", key.str(),
                                       where, expected));
            }
        }
    }

    [[nodiscard]] std::int64_t integer(const toml::node& node, std::string_view key,
                                       std::int64_t min, std::int64_t max) const {
        const auto* value = node.as_integer();
        if (value == nullptr) {
            fail(node, std::format("'{}' must be a whole number", key));
        }
        const std::int64_t result = value->get();
        if (result < min || result > max) {
            fail(node, std::format("'{}' must be between {} and {}", key, min, max));
        }
        return result;
    }

    [[nodiscard]] double number(const toml::node& node, std::string_view key) const {
        if (const auto* value = node.as_integer()) {
            return static_cast<double>(value->get());
        }
        if (const auto* value = node.as_floating_point()) {
            if (!std::isfinite(value->get())) {
                fail(node, std::format("'{}' must be a finite number", key));
            }
            return value->get();
        }
        fail(node, std::format("'{}' must be a number", key));
    }

    [[nodiscard]] std::string text(const toml::node& node, std::string_view key) const {
        const auto* value = node.as_string();
        if (value == nullptr) {
            fail(node, std::format("'{}' must be text in quotes", key));
        }
        return value->get();
    }

    [[nodiscard]] Duration duration(const toml::node& node, std::string_view key) const {
        const auto* value = node.as_string();
        if (value == nullptr) {
            fail(node, std::format("'{}' must be a duration in quotes, such as \"1.5ms\"", key));
        }
        try {
            return parseDuration(value->get());
        } catch (const std::invalid_argument& error) {
            fail(node, std::format("'{}': {}", key, error.what()));
        }
    }

    [[nodiscard]] const toml::node& required(const toml::table& table,
                                             std::string_view key) const {
        const toml::node* node = table.get(key);
        if (node == nullptr) {
            fail(table, std::format("missing '{}'", key));
        }
        return *node;
    }

    [[nodiscard]] const toml::table& table(const toml::node& node, std::string_view key) const {
        const auto* value = node.as_table();
        if (value == nullptr) {
            fail(node, std::format("'{}' must be a table, such as {{ name = value }}", key));
        }
        return *value;
    }

    [[nodiscard]] Parameters::Value parameter(const toml::node& node, std::string_view key) const {
        if (const auto* value = node.as_boolean()) {
            return value->get();
        }
        if (const auto* value = node.as_integer()) {
            return value->get();
        }
        if (const auto* value = node.as_floating_point()) {
            if (!std::isfinite(value->get())) {
                fail(node, std::format("agent parameter '{}' must be a finite number", key));
            }
            return value->get();
        }
        if (const auto* value = node.as_string()) {
            return value->get();
        }
        fail(node, std::format("agent parameter '{}' must be a number, text, or true or false",
                               key));
    }

private:
    std::string_view source_;
};

GameConfig readGame(const Reader& reader, const toml::table& table) {
    reader.allowOnly(table,
                     {"start", "length", "periods", "break", "scores", "points", "point_weights",
                      "tie", "pregame_drift", "in_play_drift", "news", "news_size", "script"},
                     "in [prediction.game]");
    GameConfig game;
    for (const auto& [key, duration] :
         {std::pair{"start", &game.start}, std::pair{"length", &game.length},
          std::pair{"break", &game.breakLength}}) {
        if (const auto* node = table.get(key)) {
            *duration = reader.duration(*node, key);
        }
    }
    if (game.length <= 0) {
        reader.fail(table, "a game's 'length' must be positive");
    }
    if (const auto* node = table.get("periods")) {
        game.periods = static_cast<int>(reader.integer(*node, "periods", 1, 100));
    }
    for (const auto& [key, number] :
         {std::pair{"scores", &game.scores}, std::pair{"tie", &game.tie},
          std::pair{"pregame_drift", &game.pregameDrift},
          std::pair{"in_play_drift", &game.inPlayDrift}, std::pair{"news", &game.news},
          std::pair{"news_size", &game.newsSize}}) {
        if (const auto* node = table.get(key)) {
            *number = reader.number(*node, key);
        }
    }
    if (const auto* node = table.get("points")) {
        const auto* list = node->as_array();
        if (list == nullptr || list->empty()) {
            reader.fail(*node, "'points' must be a list of what a score can be worth");
        }
        game.points.clear();
        for (const toml::node& element : *list) {
            game.points.push_back(static_cast<int>(reader.integer(element, "points", 1, 100)));
        }
        game.pointWeights.assign(game.points.size(), 1.0);
    }
    if (const auto* node = table.get("point_weights")) {
        const auto* list = node->as_array();
        if (list == nullptr || list->size() != game.points.size()) {
            reader.fail(*node, "'point_weights' must be a list as long as 'points'");
        }
        game.pointWeights.clear();
        for (const toml::node& element : *list) {
            game.pointWeights.push_back(reader.number(element, "point_weights"));
        }
    }
    if (const auto* node = table.get("script")) {
        const auto* list = node->as_array();
        if (list == nullptr) {
            reader.fail(*node, "'script' must be a list of scores, such as "
                               "[{ at = \"47m\", side = \"home\", points = 1 }]");
        }
        for (const toml::node& element : *list) {
            const toml::table& entry = reader.table(element, "script");
            reader.allowOnly(entry, {"at", "side", "points"}, "in a scripted score");
            ScriptedScore score{.at = reader.duration(reader.required(entry, "at"), "at")};
            const std::string side = reader.text(reader.required(entry, "side"), "side");
            if (side != "home" && side != "away") {
                reader.fail(element, "a scripted score's 'side' is \"home\" or \"away\"");
            }
            score.home = side == "home";
            if (const auto* points = entry.get("points")) {
                score.points = static_cast<int>(reader.integer(*points, "points", 1, 100));
            }
            if (!game.script.empty() && score.at < game.script.back().at) {
                reader.fail(element, "scripted scores must come in the order they were scored");
            }
            game.script.push_back(score);
        }
    }
    return game;
}

PredictionConfig readPrediction(const Reader& reader, const toml::table& table) {
    reader.allowOnly(table, {"probability", "news_rate", "news_share", "step", "game"},
                     "in [prediction]");
    PredictionConfig config;
    if (const auto* node = table.get("game")) {
        config.game = readGame(reader, reader.table(*node, "game"));
    }
    if (const auto* node = table.get("probability")) {
        config.probability = reader.number(*node, "probability");
        if (!(config.probability > 0.0 && config.probability < 1.0)) {
            reader.fail(*node, "'probability' must be between 0 and 1");
        }
    }
    if (const auto* node = table.get("news_rate")) {
        config.newsRate = reader.number(*node, "news_rate");
        if (config.newsRate < 0.0) {
            reader.fail(*node, "'news_rate' must not be negative");
        }
    }
    if (const auto* node = table.get("news_share")) {
        config.newsShare = reader.number(*node, "news_share");
        if (!(config.newsShare >= 0.0 && config.newsShare < 1.0)) {
            reader.fail(*node, "'news_share' must be at least 0 and below 1");
        }
        if (config.newsShare > 0.0 && config.newsRate == 0.0) {
            reader.fail(*node, "'news_share' needs news: give a 'news_rate'");
        }
    }
    if (const auto* node = table.get("step")) {
        config.step = reader.duration(*node, "step");
        if (config.step <= 0) {
            reader.fail(*node, "'step' must be positive");
        }
    }
    return config;
}

FundamentalConfig readFundamental(const Reader& reader, const toml::table& table,
                                  Price referencePrice) {
    reader.allowOnly(table,
                     {"initial", "mean_reversion", "volatility", "step", "jump_rate", "jump_size"},
                     "in [fundamental]");
    FundamentalConfig config{.initial = static_cast<double>(referencePrice)};
    if (const auto* node = table.get("initial")) {
        config.initial = reader.number(*node, "initial");
    }
    if (const auto* node = table.get("mean_reversion")) {
        config.meanReversion = reader.number(*node, "mean_reversion");
    }
    if (const auto* node = table.get("volatility")) {
        config.volatility = reader.number(*node, "volatility");
    }
    if (const auto* node = table.get("step")) {
        config.step = reader.duration(*node, "step");
    }
    if (const auto* node = table.get("jump_rate")) {
        config.jumpRate = reader.number(*node, "jump_rate");
    }
    if (const auto* node = table.get("jump_size")) {
        config.jumpSize = reader.number(*node, "jump_size");
    }
    return config;
}

// A fee rate given in ticks per lot, such as 0.3, in fee units.
Fee readFeeRate(const Reader& reader, const toml::node& node, std::string_view key) {
    const double units = reader.number(node, key) * static_cast<double>(kFeeUnitsPerTickLot);
    const double whole = std::round(units);
    if (std::abs(units - whole) > 1e-6 || std::abs(whole) > static_cast<double>(kMaxFeeRate)) {
        reader.fail(node, std::format("'{}' must be in ticks per lot, with at most three "
                                      "decimals and within ±{}",
                                      key, kMaxFeeRate / kFeeUnitsPerTickLot));
    }
    return static_cast<Fee>(whole);
}

// A rate given in ticks, such as 0.05, in points, which must not be negative.
Points readPoints(const Reader& reader, const toml::node& node, std::string_view key) {
    const double units = reader.number(node, key) * static_cast<double>(kPointsPerTickLot);
    const double whole = std::round(units);
    if (std::abs(units - whole) > 1e-6 || whole < 0.0 || whole > 1e15) {
        reader.fail(node, std::format("'{}' must be in ticks, with at most three decimals, and "
                                      "not negative",
                                      key));
    }
    return static_cast<Points>(whole);
}

Target readTarget(const Reader& reader, const toml::table& table) {
    reader.allowOnly(table, {"side", "quantity", "benchmark", "unfinished_penalty"},
                     "in [scoring.target]");
    Target target;
    const std::string side = reader.text(reader.required(table, "side"), "side");
    if (side != "buy" && side != "sell") {
        reader.fail(table, "'side' must be \"buy\" or \"sell\"");
    }
    target.side = side == "buy" ? Side::Buy : Side::Sell;
    target.quantity = reader.integer(reader.required(table, "quantity"), "quantity", 1,
                                     kMaxQuantity);
    if (const auto* node = table.get("benchmark")) {
        const std::string benchmark = reader.text(*node, "benchmark");
        if (benchmark == "vwap") {
            target.benchmark = Benchmark::Vwap;
        } else if (benchmark == "reference") {
            target.benchmark = Benchmark::Reference;
        } else {
            reader.fail(*node, "'benchmark' must be \"vwap\" or \"reference\"");
        }
    }
    if (const auto* node = table.get("unfinished_penalty")) {
        target.unfinishedPenalty = readPoints(reader, *node, "unfinished_penalty");
    }
    return target;
}

Mark readMark(const Reader& reader, const toml::node& node, std::string_view key) {
    const std::string mark = reader.text(node, key);
    if (mark == "last") {
        return Mark::LastTrade;
    }
    if (mark == "value") {
        return Mark::Value;
    }
    if (mark != "book") {
        reader.fail(node, std::format("'{}' must be \"last\", \"value\" or \"book\"", key));
    }
    return Mark::Book;
}

ScoringConfig readScoring(const Reader& reader, const toml::table& table) {
    reader.allowOnly(table,
                     {"mark", "inventory_penalty", "close_penalty", "max_loss", "target"},
                     "in [scoring]");
    ScoringConfig config;
    if (const auto* node = table.get("mark")) {
        config.mark = readMark(reader, *node, "mark");
    }
    if (const auto* node = table.get("inventory_penalty")) {
        config.inventoryPenalty = readPoints(reader, *node, "inventory_penalty");
    }
    if (const auto* node = table.get("close_penalty")) {
        config.closePenalty = readPoints(reader, *node, "close_penalty");
    }
    if (const auto* node = table.get("max_loss")) {
        config.maxLoss = reader.integer(*node, "max_loss", 0, kMaxCashLimit);
    }
    if (const auto* node = table.get("target")) {
        config.target = readTarget(reader, reader.table(*node, "target"));
    }
    return config;
}

Challenge readChallenge(const Reader& reader, const toml::table& table) {
    reader.allowOnly(table, {"name", "briefing"}, "in [challenge]");
    Challenge challenge{.name = reader.text(reader.required(table, "name"), "name"),
                        .briefing = reader.text(reader.required(table, "briefing"), "briefing")};
    if (challenge.name.empty()) {
        reader.fail(table, "a challenge needs a name");
    }
    return challenge;
}

TradingDay readTradingDay(const Reader& reader, const toml::table& table) {
    reader.allowOnly(table,
                     {"opening_auction", "closing_auction", "halt_band", "halt", "activity"},
                     "in [trading_day]");
    TradingDay day;
    if (const auto* node = table.get("opening_auction")) {
        day.openingAuction = reader.duration(*node, "opening_auction");
    }
    if (const auto* node = table.get("closing_auction")) {
        day.closingAuction = reader.duration(*node, "closing_auction");
    }
    if (const auto* node = table.get("halt_band")) {
        day.haltBand = reader.integer(*node, "halt_band", 0, kMaxPrice);
    }
    if (const auto* node = table.get("halt")) {
        day.halt = reader.duration(*node, "halt");
    }
    if (day.haltBand > 0 && day.halt == 0) {
        reader.fail(table, "a halt_band needs a halt, such as halt = \"30s\"");
    }
    if (const auto* node = table.get("activity")) {
        day.activity = reader.number(*node, "activity");
        if (day.activity < 0.0) {
            reader.fail(*node, "'activity' must not be negative");
        }
    }
    return day;
}

ExchangeConfig readExchange(const Reader& reader, const toml::table& table) {
    reader.allowOnly(table, {"depth_levels", "maker_fee", "taker_fee", "auction_fee", "fee_curve"},
                     "in [exchange]");
    ExchangeConfig config;
    if (const auto* node = table.get("depth_levels")) {
        config.depthLevels = static_cast<std::size_t>(reader.integer(
            *node, "depth_levels", 0, static_cast<std::int64_t>(kMaxDepthLevels)));
    }
    if (const auto* node = table.get("maker_fee")) {
        config.makerFee = readFeeRate(reader, *node, "maker_fee");
    }
    if (const auto* node = table.get("taker_fee")) {
        config.takerFee = readFeeRate(reader, *node, "taker_fee");
    }
    if (const auto* node = table.get("auction_fee")) {
        config.auctionFee = readFeeRate(reader, *node, "auction_fee");
        if (config.auctionFee < 0) {
            reader.fail(*node, "'auction_fee' must not be negative: both sides of an auction "
                               "trade pay it");
        }
    }
    if (config.makerFee + config.takerFee < 0) {
        reader.fail(table, "maker_fee plus taker_fee must not be negative, or the exchange would "
                           "pay out more than it collects on every trade");
    }
    if (const auto* node = table.get("fee_curve")) {
        const std::string curve = reader.text(*node, "fee_curve");
        if (curve == "probability") {
            config.feeCurve = FeeCurve::Probability;
        } else if (curve != "flat") {
            reader.fail(*node, "'fee_curve' must be \"flat\" or \"probability\"");
        }
    }
    return config;
}

// A link's delay: a duration, or a distribution as { quantiles = [...] }, durations in order
// evenly spaced in probability, whose median stands for the link where one number is needed.
void readDelay(const Reader& reader, const toml::node& node, std::string_view key,
               Duration& delay, std::vector<Duration>& quantiles) {
    if (!node.is_table()) {
        delay = reader.duration(node, key);
        return;
    }
    const toml::table& table = *node.as_table();
    reader.allowOnly(table, {"quantiles"}, std::format("in '{}'", key));
    const toml::node& list = reader.required(table, "quantiles");
    const auto* values = list.as_array();
    if (values == nullptr || values->empty()) {
        reader.fail(list, std::format("'{}' quantiles must be a list of one or more durations, "
                                      "shortest first",
                                      key));
    }
    for (const toml::node& element : *values) {
        const Duration value = reader.duration(element, key);
        if (!quantiles.empty() && value < quantiles.back()) {
            reader.fail(element, std::format("'{}' quantiles must be in order, shortest first",
                                             key));
        }
        quantiles.push_back(value);
    }
    delay = medianOf(quantiles);
}

Latency readLatency(const Reader& reader, const toml::table& table) {
    reader.allowOnly(table, {"to_exchange", "from_exchange", "jitter"}, "in latency");
    Latency latency;
    if (const auto* node = table.get("to_exchange")) {
        readDelay(reader, *node, "to_exchange", latency.toExchange, latency.toExchangeQuantiles);
    }
    if (const auto* node = table.get("from_exchange")) {
        readDelay(reader, *node, "from_exchange", latency.fromExchange,
                  latency.fromExchangeQuantiles);
    }
    if (const auto* node = table.get("jitter")) {
        latency.jitter = reader.duration(*node, "jitter");
    }
    return latency;
}

AccountConfig readAccount(const Reader& reader, const toml::table& table) {
    reader.allowOnly(table,
                     {"initial_cash", "initial_position", "max_position", "max_order_quantity"},
                     "in account");
    constexpr auto kMaxCash = std::numeric_limits<Cash>::max();
    AccountConfig account;
    if (const auto* node = table.get("initial_cash")) {
        account.initialCash = reader.integer(*node, "initial_cash", -kMaxCash, kMaxCash);
    }
    if (const auto* node = table.get("initial_position")) {
        account.initialPosition =
            reader.integer(*node, "initial_position", -kMaxQuantity, kMaxQuantity);
    }
    if (const auto* node = table.get("max_position")) {
        account.maxPosition = reader.integer(*node, "max_position", 0, kMaxQuantity);
    }
    if (const auto* node = table.get("max_order_quantity")) {
        account.maxOrderQuantity = reader.integer(*node, "max_order_quantity", 1, kMaxQuantity);
    }
    return account;
}

AgentOptions readParticipant(const Reader& reader, const toml::table& table) {
    reader.allowOnly(table, {"latency", "account"}, "in [participant]");
    AgentOptions options;
    if (const auto* node = table.get("latency")) {
        options.latency = readLatency(reader, reader.table(*node, "latency"));
    }
    if (const auto* node = table.get("account")) {
        options.account = readAccount(reader, reader.table(*node, "account"));
    }
    return options;
}

LearningFeed readFeed(const Reader& reader, const toml::table& table) {
    reader.allowOnly(table, {"interval", "lag", "noise", "trigger"}, "in learning.feed");
    LearningFeed feed;
    if (const auto* node = table.get("interval")) {
        feed.interval = reader.duration(*node, "interval");
        if (feed.interval < kMinFeedInterval) {
            reader.fail(*node, "the feed's 'interval' must be at least 1ms");
        }
    }
    if (const auto* node = table.get("lag")) {
        feed.lag = reader.duration(*node, "lag");
    }
    if (const auto* node = table.get("noise")) {
        feed.noise = reader.number(*node, "noise");
        if (feed.noise < 0.0) {
            reader.fail(*node, "'noise' must not be negative");
        }
    }
    if (const auto* node = table.get("trigger")) {
        feed.trigger = reader.number(*node, "trigger");
        if (feed.trigger < 0.0) {
            reader.fail(*node, "'trigger' must not be negative");
        }
    }
    return feed;
}

// One end of a range: a whole number, a number, or a duration written as text.
Parameters::Value rangeEnd(const Reader& reader, const toml::node& node, std::string_view path) {
    if (const auto* value = node.as_integer()) {
        return value->get();
    }
    if (node.is_floating_point()) {
        return reader.number(node, path);
    }
    if (node.is_string()) {
        static_cast<void>(reader.duration(node, path));
        return reader.text(node, path);
    }
    reader.fail(node, std::format("the range for '{}' must run between two numbers or two "
                                  "durations",
                                  path));
}

SettingRange readRange(const Reader& reader, const toml::node& node,
                       const std::vector<std::string>& keys, SettingRange::Form form) {
    SettingRange range{.keys = keys, .form = form};
    const std::string path = range.path();
    const auto* ends = node.as_array();
    if (ends == nullptr || ends->size() != 2) {
        reader.fail(node, std::format("the range for '{}' must be [low, high]", path));
    }
    range.low = rangeEnd(reader, *ends->get(0), path);
    range.high = rangeEnd(reader, *ends->get(1), path);
    const bool numbers = !std::holds_alternative<std::string>(range.low) &&
                         !std::holds_alternative<std::string>(range.high);
    const bool durations = std::holds_alternative<std::string>(range.low) &&
                           std::holds_alternative<std::string>(range.high);
    if (!numbers && !durations) {
        reader.fail(node, std::format("the range for '{}' must run between two numbers or two "
                                      "durations",
                                      path));
    }
    const auto asNumber = [](const Parameters::Value& value) {
        if (const auto* whole = std::get_if<std::int64_t>(&value)) {
            return static_cast<double>(*whole);
        }
        return std::get<double>(value);
    };
    if (numbers && (std::holds_alternative<double>(range.low) ||
                    std::holds_alternative<double>(range.high))) {
        // A decimal at either end makes the range one of numbers, not whole numbers.
        range.low = asNumber(range.low);
        range.high = asNumber(range.high);
    }
    const bool ordered =
        durations ? parseDuration(std::get<std::string>(range.low)) <=
                        parseDuration(std::get<std::string>(range.high))
                  : asNumber(range.low) <= asNumber(range.high);
    if (!ordered) {
        reader.fail(node, std::format("the range for '{}' must not run from high to low", path));
    }
    if (form == SettingRange::Form::Log) {
        if (durations || !(asNumber(range.low) > 0.0)) {
            reader.fail(node, std::format("a log range, as for '{}', runs between two positive "
                                          "numbers",
                                          path));
        }
        range.low = asNumber(range.low);
        range.high = asNumber(range.high);
    }
    return range;
}

// The ranges under a table of [learning.ranges], whose keys mirror the scenario's.
void readRanges(const Reader& reader, const toml::table& table, std::vector<std::string>& keys,
                std::vector<SettingRange>& ranges) {
    for (auto&& [key, node] : table) {
        keys.emplace_back(key.str());
        const std::string path = SettingRange{.keys = keys}.path();
        if (node.is_array()) {
            ranges.push_back(readRange(reader, node, keys, SettingRange::Form::Uniform));
        } else if (const auto* inner = node.as_table();
                   inner != nullptr && (inner->contains("log") || inner->contains("choices") ||
                                        inner->contains("range"))) {
            reader.allowOnly(*inner, {"log", "choices", "range", "step"},
                             std::format("in the range for '{}'", path));
            const std::size_t forms = (inner->contains("log") ? 1U : 0U) +
                                      (inner->contains("choices") ? 1U : 0U) +
                                      (inner->contains("range") ? 1U : 0U);
            if (forms != 1 || (inner->contains("step") && !inner->contains("range"))) {
                reader.fail(node, std::format("the range for '{}' is one of range with a step, log "
                                              "or choices",
                                              path));
            }
            if (const auto* rangeNode = inner->get("range")) {
                SettingRange stepped =
                    readRange(reader, *rangeNode, keys, SettingRange::Form::Uniform);
                const toml::node& stepNode = reader.required(*inner, "step");
                Parameters::Value step = rangeEnd(reader, stepNode, path);
                if (std::holds_alternative<std::string>(step) !=
                    std::holds_alternative<std::string>(stepped.low)) {
                    reader.fail(stepNode, std::format("the step for '{}' must be a number for a "
                                                      "range of numbers and a duration for one of "
                                                      "durations",
                                                      path));
                }
                if (const auto* whole = std::get_if<std::int64_t>(&step);
                    whole != nullptr && std::holds_alternative<double>(stepped.low)) {
                    step = static_cast<double>(*whole);
                }
                if (std::holds_alternative<double>(step) &&
                    std::holds_alternative<std::int64_t>(stepped.low)) {
                    // A decimal step makes the range one of numbers.
                    stepped.low = static_cast<double>(std::get<std::int64_t>(stepped.low));
                    stepped.high = static_cast<double>(std::get<std::int64_t>(stepped.high));
                }
                const bool positive =
                    std::visit([](const auto& value) {
                        using T = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<T, std::string>) {
                            return parseDuration(value) > 0;
                        } else if constexpr (std::is_same_v<T, bool>) {
                            return false;
                        } else {
                            return value > 0;
                        }
                    }, step);
                if (!positive) {
                    reader.fail(stepNode, std::format("the step for '{}' must be positive", path));
                }
                stepped.step = step;
                ranges.push_back(std::move(stepped));
            } else if (const auto* log = inner->get("log")) {
                ranges.push_back(readRange(reader, *log, keys, SettingRange::Form::Log));
            } else {
                const auto* choices = inner->get("choices")->as_array();
                if (choices == nullptr || choices->empty()) {
                    reader.fail(node, std::format("the choices for '{}' must be a list of one or "
                                                  "more values",
                                                  path));
                }
                SettingRange range{.keys = keys, .form = SettingRange::Form::Choices};
                for (const toml::node& choice : *choices) {
                    if (choice.is_table() || choice.is_array()) {
                        reader.fail(choice, std::format("a choice for '{}' must be a number, "
                                                        "text, or true or false",
                                                        path));
                    }
                    range.choices.push_back(reader.parameter(choice, path));
                }
                ranges.push_back(std::move(range));
            }
        } else if (inner != nullptr) {
            readRanges(reader, *inner, keys, ranges);
        } else {
            reader.fail(node, std::format("the range for '{}' must be [low, high], "
                                          "{{ log = [low, high] }} or {{ choices = [...] }}",
                                          path));
        }
        keys.pop_back();
    }
}

LearningConfig readLearning(const Reader& reader, const toml::table& table,
                            const Scenario& scenario) {
    reader.allowOnly(table,
                     {"interval", "warmup", "actions", "sizes", "take_ticks", "reward_mark",
                      "close", "max_events", "max_depth_updates", "depth_levels", "max_orders",
                      "feed", "ranges"},
                     "in [learning]");
    const bool hasValue = scenario.fundamental || scenario.prediction;
    LearningConfig config;
    if (const auto* node = table.get("interval")) {
        config.interval = reader.duration(*node, "interval");
        if (config.interval <= 0) {
            reader.fail(*node, "'interval' must be positive");
        }
    }
    if (const auto* node = table.get("warmup")) {
        config.warmup = reader.duration(*node, "warmup");
    }
    if (config.warmup >= scenario.duration) {
        reader.fail(table, "the first decision, at 'warmup', must come before the end of the run");
    }
    if (const auto* node = table.get("actions")) {
        const auto* list = node->as_array();
        if (list == nullptr || list->empty()) {
            reader.fail(*node, "'actions' must be a list of one or more action names");
        }
        config.actions.clear();
        for (const toml::node& element : *list) {
            const std::string name = reader.text(element, "actions");
            const auto action = learningActionNamed(name);
            if (!action) {
                reader.fail(element, std::format("there is no action '{}'", name));
            }
            if (std::ranges::find(config.actions, *action) != config.actions.end()) {
                reader.fail(element, std::format("action '{}' is listed twice", name));
            }
            config.actions.push_back(*action);
        }
    }
    if (const auto* node = table.get("sizes")) {
        const auto* list = node->as_array();
        if (list == nullptr || list->empty()) {
            reader.fail(*node, "'sizes' must be a list of one or more order sizes");
        }
        config.sizes.clear();
        for (const toml::node& element : *list) {
            config.sizes.push_back(reader.integer(element, "sizes", 1, kMaxQuantity));
        }
    }
    if (const auto* node = table.get("take_ticks")) {
        config.takeTicks = reader.integer(*node, "take_ticks", 0, kMaxPrice);
    }
    for (const auto& [key, mark] : {std::pair{"reward_mark", &config.rewardMark},
                                    std::pair{"close", &config.close}}) {
        if (const auto* node = table.get(key)) {
            *mark = readMark(reader, *node, key);
            if (*mark == Mark::Value && !hasValue) {
                reader.fail(*node, "valuing at the true value needs a [fundamental] or "
                                   "[prediction] section");
            }
        }
    }
    const auto count = [&](std::string_view key, std::size_t& value, std::int64_t min,
                           std::int64_t max) {
        if (const auto* node = table.get(key)) {
            value = static_cast<std::size_t>(reader.integer(*node, key, min, max));
        }
    };
    count("max_events", config.maxEvents, 1, 65'536);
    count("max_depth_updates", config.maxDepthUpdates, 0, 4'096);
    count("depth_levels", config.depthLevels, 0, static_cast<std::int64_t>(kMaxDepthLevels));
    count("max_orders", config.maxOrders, 1, 1'024);
    if (const auto* node = table.get("feed")) {
        if (!hasValue) {
            reader.fail(*node, "a feed of the underlying needs a [fundamental] or [prediction] "
                               "section");
        }
        config.feed = readFeed(reader, reader.table(*node, "feed"));
    }
    if (const auto* node = table.get("ranges")) {
        std::vector<std::string> keys;
        readRanges(reader, reader.table(*node, "ranges"), keys, config.ranges);
    }
    return config;
}

AgentGroup readGroup(const Reader& reader, const toml::table& table) {
    AgentGroup group;
    for (auto&& [key, node] : table) {
        const std::string_view name = key.str();
        if (name == "type") {
            group.type = reader.text(node, name);
        } else if (name == "name") {
            group.name = reader.text(node, name);
        } else if (name == "count") {
            group.count = reader.integer(node, name, 1, 1'000'000);
        } else if (name == "start") {
            group.options.startTime = reader.duration(node, name);
        } else if (name == "latency") {
            group.options.latency = readLatency(reader, reader.table(node, name));
        } else if (name == "account") {
            group.options.account = readAccount(reader, reader.table(node, name));
        } else {
            group.parameters.set(std::string{name}, reader.parameter(node, name));
        }
    }
    if (group.type.empty()) {
        reader.fail(table, "every [[agents]] table needs a type, such as "
                           "type = \"zero_intelligence\"");
    }
    return group;
}

} // namespace

Scenario detail::readScenario(const toml::table& root, std::string_view source) {
    const Reader reader{source};
    reader.allowOnly(root,
                     {"scenario_version", "seed", "duration", "reference_price", "fundamental",
                      "prediction", "exchange", "trading_day", "agents", "participant",
                      "scoring", "challenge", "learning"},
                     "at the top level");
    if (const auto* node = root.get("scenario_version")) {
        const auto* value = node->as_integer();
        if (value == nullptr || value->get() < 1) {
            reader.fail(*node, "'scenario_version' must be a whole number from 1");
        }
        if (value->get() > kScenarioVersion) {
            reader.fail(*node, std::format("this scenario is for a newer crowdbook: it has "
                                           "scenario_version {}, and this one reads up to {}",
                                           value->get(), kScenarioVersion));
        }
    }

    Scenario scenario;
    if (const auto* node = root.get("seed")) {
        scenario.seed = static_cast<std::uint64_t>(
            reader.integer(*node, "seed", 0, std::numeric_limits<std::int64_t>::max()));
    }
    if (const auto* node = root.get("duration")) {
        scenario.duration = reader.duration(*node, "duration");
    }
    if (const auto* node = root.get("reference_price")) {
        scenario.referencePrice = reader.integer(*node, "reference_price", 1, kMaxPrice);
    }
    if (const auto* node = root.get("fundamental")) {
        scenario.fundamental =
            readFundamental(reader, reader.table(*node, "fundamental"), scenario.referencePrice);
    }
    if (const auto* node = root.get("prediction")) {
        if (scenario.fundamental) {
            reader.fail(*node, "a prediction market's true value is its probability of yes: "
                               "drop the [fundamental] section");
        }
        scenario.prediction = readPrediction(reader, reader.table(*node, "prediction"));
        // Agents anchor at the starting probability, in cents, unless told otherwise.
        if (const auto* price = root.get("reference_price")) {
            if (scenario.referencePrice >= kPredictionPayout) {
                reader.fail(*price, std::format("a prediction market trades from 1 to {}",
                                                kPredictionPayout - 1));
            }
        } else {
            scenario.referencePrice = std::clamp<Price>(
                std::llround(scenario.prediction->probability * kPredictionPayout), 1,
                kPredictionPayout - 1);
        }
    }
    if (const auto* node = root.get("exchange")) {
        const toml::table& table = reader.table(*node, "exchange");
        scenario.exchange = readExchange(reader, table);
        if (scenario.exchange.feeCurve == FeeCurve::Probability && !scenario.prediction) {
            reader.fail(*table.get("fee_curve"),
                        "fee_curve = \"probability\" needs a market whose shares pay out: add a "
                        "[prediction] section");
        }
    }
    if (const auto* node = root.get("trading_day")) {
        scenario.tradingDay = readTradingDay(reader, reader.table(*node, "trading_day"));
        if (scenario.tradingDay->openingAuction + scenario.tradingDay->closingAuction >
            scenario.duration) {
            reader.fail(*node, "the opening and closing auctions must fit in the duration");
        }
    }
    if (const auto* node = root.get("participant")) {
        scenario.participant = readParticipant(reader, reader.table(*node, "participant"));
    }
    if (const auto* node = root.get("scoring")) {
        scenario.scoring = readScoring(reader, reader.table(*node, "scoring"));
        if (scenario.scoring->mark == Mark::Value && !scenario.fundamental &&
            !scenario.prediction) {
            reader.fail(*node,
                        "scoring at the true value needs a [fundamental] or [prediction] section");
        }
    }
    if (const auto* node = root.get("challenge")) {
        scenario.challenge = readChallenge(reader, reader.table(*node, "challenge"));
    }
    if (const auto* node = root.get("learning")) {
        scenario.learning = readLearning(reader, reader.table(*node, "learning"), scenario);
    }
    if (const auto* node = root.get("prediction")) {
        // Some limits depend on the duration as well, such as how much news can come.
        try {
            checkFundamental(scenario);
        } catch (const ScenarioError& error) {
            reader.fail(*node, error.what());
        }
    }
    if (const auto* node = root.get("agents")) {
        const auto* list = node->as_array();
        if (list == nullptr || !list->is_array_of_tables()) {
            reader.fail(*node, "agents must be written as [[agents]] tables");
        }
        for (const toml::node& element : *list) {
            scenario.groups.push_back(readGroup(reader, *element.as_table()));
        }
    }
    if (scenario.groups.empty()) {
        throw ScenarioError(std::format("{}: the scenario has no [[agents]]", source));
    }
    return scenario;
}

Scenario parseScenario(std::string_view text, std::string_view source) {
    try {
        return detail::readScenario(toml::parse(text, source), source);
    } catch (const toml::parse_error& error) {
        throw ScenarioError(
            std::format("{}:{}: {}", source, error.source().begin.line, error.description()));
    }
}

Scenario loadScenario(const std::filesystem::path& path) {
    std::ifstream file{path};
    if (!file) {
        throw ScenarioError(std::format("cannot read '{}'", path.string()));
    }
    std::ostringstream text;
    text << file.rdbuf();
    return parseScenario(text.str(), path.string());
}

} // namespace crowdbook

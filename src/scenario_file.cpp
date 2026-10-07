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

#include <toml++/toml.hpp>

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

ScoringConfig readScoring(const Reader& reader, const toml::table& table) {
    reader.allowOnly(table,
                     {"mark", "inventory_penalty", "close_penalty", "max_loss", "target"},
                     "in [scoring]");
    ScoringConfig config;
    if (const auto* node = table.get("mark")) {
        const std::string mark = reader.text(*node, "mark");
        if (mark == "last") {
            config.mark = Mark::LastTrade;
        } else if (mark == "value") {
            config.mark = Mark::Value;
        } else {
            reader.fail(*node, "'mark' must be \"last\" or \"value\"");
        }
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

ExchangeConfig readExchange(const Reader& reader, const toml::table& table) {
    reader.allowOnly(table, {"depth_levels", "maker_fee", "taker_fee"}, "in [exchange]");
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
    if (config.makerFee + config.takerFee < 0) {
        reader.fail(table, "maker_fee plus taker_fee must not be negative, or the exchange would "
                           "pay out more than it collects on every trade");
    }
    return config;
}

Latency readLatency(const Reader& reader, const toml::table& table) {
    reader.allowOnly(table, {"to_exchange", "from_exchange", "jitter"}, "in latency");
    Latency latency;
    if (const auto* node = table.get("to_exchange")) {
        latency.toExchange = reader.duration(*node, "to_exchange");
    }
    if (const auto* node = table.get("from_exchange")) {
        latency.fromExchange = reader.duration(*node, "from_exchange");
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

Scenario parseScenario(std::string_view text, std::string_view source) {
    toml::table root;
    try {
        root = toml::parse(text, source);
    } catch (const toml::parse_error& error) {
        throw ScenarioError(
            std::format("{}:{}: {}", source, error.source().begin.line, error.description()));
    }

    const Reader reader{source};
    reader.allowOnly(root,
                     {"seed", "duration", "reference_price", "fundamental", "exchange", "agents",
                      "participant", "scoring", "challenge"},
                     "at the top level");

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
    if (const auto* node = root.get("exchange")) {
        scenario.exchange = readExchange(reader, reader.table(*node, "exchange"));
    }
    if (const auto* node = root.get("participant")) {
        scenario.participant = readParticipant(reader, reader.table(*node, "participant"));
    }
    if (const auto* node = root.get("scoring")) {
        scenario.scoring = readScoring(reader, reader.table(*node, "scoring"));
        if (scenario.scoring->mark == Mark::Value && !scenario.fundamental) {
            reader.fail(*node, "scoring at the true value needs a [fundamental] section");
        }
    }
    if (const auto* node = root.get("challenge")) {
        scenario.challenge = readChallenge(reader, reader.table(*node, "challenge"));
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

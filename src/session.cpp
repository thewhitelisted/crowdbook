#include "crowdbook/session.hpp"

#include <array>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <variant>

#include <toml++/toml.hpp>

#include "crowdbook/scenario_file.hpp"

namespace crowdbook {

namespace {

constexpr std::int64_t kSessionVersion = 1;

// A TOML basic string, escaped.
std::string tomlString(std::string_view text) {
    std::string quoted = "\"";
    for (const char c : text) {
        switch (c) {
        case '"':
            quoted += "\\\"";
            break;
        case '\\':
            quoted += "\\\\";
            break;
        case '\n':
            quoted += "\\n";
            break;
        case '\t':
            quoted += "\\t";
            break;
        case '\r':
            quoted += "\\r";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
                quoted += std::format("\\u{:04x}", static_cast<unsigned>(c));
            } else {
                quoted += c;
            }
        }
    }
    return quoted + '"';
}

std::string actionLine(const SessionAction& action) {
    std::string line = std::format("    {{ time_ns = {}, instrument = {}, ", action.time,
                                   action.instrument);
    if (const auto* order = std::get_if<NewOrder>(&action.request)) {
        line += std::format("request = \"new\", client_order_id = {}, side = \"{}\", type = \"{}\"",
                            order->clientOrderId, toString(order->side), toString(order->type));
        if (order->type == OrderType::Limit) {
            line += std::format(", time_in_force = \"{}\", price = {}",
                                toString(order->timeInForce), order->price);
        }
        line += std::format(", quantity = {} }},", order->quantity);
    } else if (const auto* cancel = std::get_if<CancelOrder>(&action.request)) {
        line += std::format("request = \"cancel\", client_order_id = {} }},",
                            cancel->clientOrderId);
    } else {
        const auto& modify = std::get<ModifyOrder>(action.request);
        line += std::format(
            "request = \"modify\", client_order_id = {}, price = {}, quantity = {} }},",
            modify.clientOrderId, modify.price, modify.quantity);
    }
    return line;
}

// Reads one TOML document, reporting problems as "source:line: message".
class Reader {
public:
    explicit Reader(std::string_view source) noexcept : source_(source) {}

    [[noreturn]] void fail(const toml::node& node, std::string_view message) const {
        throw ScenarioError(std::format("{}:{}: {}", source_, node.source().begin.line, message));
    }

    const toml::node& required(const toml::table& table, std::string_view key) const {
        const toml::node* node = table.get(key);
        if (node == nullptr) {
            fail(table, std::format("missing '{}'", key));
        }
        return *node;
    }

    std::int64_t integer(const toml::node& node, std::string_view key, std::int64_t min,
                         std::int64_t max) const {
        const auto* value = node.as_integer();
        if (value == nullptr || value->get() < min || value->get() > max) {
            fail(node, std::format("'{}' must be a whole number between {} and {}", key, min, max));
        }
        return value->get();
    }

    std::string text(const toml::node& node, std::string_view key) const {
        const auto* value = node.as_string();
        if (value == nullptr) {
            fail(node, std::format("'{}' must be text in quotes", key));
        }
        return value->get();
    }

    // One of `names`, whose index picks the value.
    template <typename Enum, std::size_t N>
    Enum choice(const toml::node& node, std::string_view key,
                const std::array<std::pair<std::string_view, Enum>, N>& names) const {
        const std::string value = text(node, key);
        for (const auto& [name, result] : names) {
            if (name == value) {
                return result;
            }
        }
        fail(node, std::format("'{}' cannot be '{}'", key, value));
    }

private:
    std::string_view source_;
};

constexpr std::int64_t kMaxInt = std::numeric_limits<std::int64_t>::max();

SessionAction readAction(const Reader& reader, const toml::table& table) {
    SessionAction action{
        .time = reader.integer(reader.required(table, "time_ns"), "time_ns", 0, kMaxInt),
        .instrument = static_cast<std::uint32_t>(
            reader.integer(reader.required(table, "instrument"), "instrument", 0, 0))};
    const auto clientOrderId = static_cast<ClientOrderId>(reader.integer(
        reader.required(table, "client_order_id"), "client_order_id", 0, kMaxInt));
    const std::string kind = reader.text(reader.required(table, "request"), "request");
    if (kind == "new") {
        NewOrder order{
            .clientOrderId = clientOrderId,
            .side = reader.choice(reader.required(table, "side"), "side",
                                  std::array{std::pair{std::string_view{"buy"}, Side::Buy},
                                             std::pair{std::string_view{"sell"}, Side::Sell}}),
            .type = reader.choice(
                reader.required(table, "type"), "type",
                std::array{std::pair{std::string_view{"limit"}, OrderType::Limit},
                           std::pair{std::string_view{"market"}, OrderType::Market}}),
            .quantity = reader.integer(reader.required(table, "quantity"), "quantity", 1,
                                       kMaxQuantity)};
        if (order.type == OrderType::Limit) {
            order.timeInForce = reader.choice(
                reader.required(table, "time_in_force"), "time_in_force",
                std::array{
                    std::pair{std::string_view{"good-till-cancel"}, TimeInForce::GoodTillCancel},
                    std::pair{std::string_view{"immediate-or-cancel"},
                              TimeInForce::ImmediateOrCancel},
                    std::pair{std::string_view{"post-only"}, TimeInForce::PostOnly}});
            order.price = reader.integer(reader.required(table, "price"), "price", 1, kMaxPrice);
        }
        action.request = order;
    } else if (kind == "cancel") {
        action.request = CancelOrder{.clientOrderId = clientOrderId};
    } else if (kind == "modify") {
        action.request = ModifyOrder{
            .clientOrderId = clientOrderId,
            .price = reader.integer(reader.required(table, "price"), "price", 1, kMaxPrice),
            .quantity =
                reader.integer(reader.required(table, "quantity"), "quantity", 1, kMaxQuantity)};
    } else {
        reader.fail(table, std::format("'request' must be new, cancel or modify, not '{}'", kind));
    }
    return action;
}

} // namespace

SessionMarket openSession(const Scenario& scenario, const AgentRegistry& registry,
                          EventSink* sink) {
    SessionMarket market{.run = ScenarioRun{scenario, registry, sink}};
    auto participant = std::make_unique<Participant>();
    market.agent = participant.get();
    try {
        market.participant = market.run.addAgent(std::string{kParticipantGroup}, "participant",
                                                 std::move(participant), scenario.participant);
    } catch (const std::invalid_argument& error) {
        throw ScenarioError(std::format("participant: {}", error.what()));
    }
    return market;
}

void writeSession(std::ostream& out, const Session& session) {
    out << "# A crowdbook session. Replay it with: crowdbook replay <this file>\n";
    out << std::format("session_version = {}\nseed = {}\nend_ns = {}\nscenario = {}\n",
                       kSessionVersion, session.seed, session.end, tomlString(session.scenario));
    out << "actions = [\n";
    for (const SessionAction& action : session.actions) {
        out << actionLine(action) << '\n';
    }
    out << "]\n";
}

Session parseSession(std::string_view text, std::string_view source) {
    toml::table root;
    try {
        root = toml::parse(text, source);
    } catch (const toml::parse_error& error) {
        throw ScenarioError(
            std::format("{}:{}: {}", source, error.source().begin.line, error.description()));
    }
    const Reader reader{source};
    reader.integer(reader.required(root, "session_version"), "session_version", kSessionVersion,
                   kSessionVersion);

    Session session{
        .scenario = reader.text(reader.required(root, "scenario"), "scenario"),
        .seed = static_cast<std::uint64_t>(
            reader.integer(reader.required(root, "seed"), "seed", 0, kMaxInt)),
        .end = reader.integer(reader.required(root, "end_ns"), "end_ns", 0, kMaxInt)};
    static_cast<void>(parseScenario(session.scenario, std::format("{} (its scenario)", source)));

    if (const toml::node* node = root.get("actions")) {
        const auto* list = node->as_array();
        if (list == nullptr) {
            reader.fail(*node, "'actions' must be a list of { ... } tables");
        }
        for (const toml::node& element : *list) {
            const auto* table = element.as_table();
            if (table == nullptr) {
                reader.fail(element, "each action must be a { ... } table");
            }
            const SessionAction& action = session.actions.emplace_back(readAction(reader, *table));
            const Timestamp latest =
                session.actions.size() > 1 ? session.actions[session.actions.size() - 2].time : 0;
            if (action.time < latest || action.time > session.end) {
                reader.fail(element, "actions must be in time order, and none after end_ns");
            }
        }
    }
    return session;
}

Session loadSession(const std::filesystem::path& path) {
    std::ifstream file{path};
    if (!file) {
        throw ScenarioError(std::format("cannot read '{}'", path.string()));
    }
    std::ostringstream text;
    text << file.rdbuf();
    return parseSession(text.str(), path.string());
}

RunResult replaySession(const Session& session, const AgentRegistry& registry, EventSink* sink) {
    Scenario scenario = parseScenario(session.scenario, "the session's scenario");
    scenario.seed = session.seed;
    SessionMarket market = openSession(scenario, registry, sink);
    for (const SessionAction& action : session.actions) {
        market.run.runUntil(action.time);
        static_cast<void>(perform(market.run.simulation(), market.participant, action));
    }
    market.run.runUntil(session.end);
    return market.run.result();
}

} // namespace crowdbook

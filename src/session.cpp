#include "crowdbook/session.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <variant>

#include <toml++/toml.hpp>

#include "crowdbook/protocol.hpp"
#include "crowdbook/scenario_file.hpp"

namespace crowdbook {

namespace {

// Version 1 had a single seat, the participant "you", and no seat in its actions.
constexpr std::int64_t kSessionVersion = 2;

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
    std::string line = std::format("    {{ time_ns = {}, seat = {}, instrument = {}, ",
                                   action.time, action.seat, action.instrument);
    if (const auto* order = std::get_if<NewOrder>(&action.request)) {
        line += std::format("request = \"new\", client_order_id = {}, side = \"{}\", type = \"{}\"",
                            order->clientOrderId, toString(order->side), toString(order->type));
        if (order->type == OrderType::Limit) {
            line += std::format(", time_in_force = \"{}\", price = {}",
                                toString(order->timeInForce), order->price);
        }
        line += std::format(", quantity = {}", order->quantity);
        if (order->parent != 0) {
            line += std::format(", parent = {}", order->parent);
        }
        line += " },";
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

// Reads one action of a session with `seats` seats; version 1 actions name none.
SessionAction readAction(const Reader& reader, const toml::table& table, std::int64_t version,
                         std::size_t seats) {
    SessionAction action{
        .time = reader.integer(reader.required(table, "time_ns"), "time_ns", 0, kMaxInt),
        .seat = version == 1 ? 0U
                             : static_cast<std::uint32_t>(reader.integer(
                                   reader.required(table, "seat"), "seat", 0,
                                   static_cast<std::int64_t>(seats) - 1)),
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
        if (const toml::node* parent = table.get("parent")) {
            order.parent =
                static_cast<std::uint64_t>(reader.integer(*parent, "parent", 0, kMaxInt));
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
                          EventSink* sink, const std::vector<std::string>& seats) {
    if (seats.empty()) {
        throw ScenarioError("a session needs at least one seat");
    }
    for (std::size_t i = 0; i < seats.size(); ++i) {
        if (std::find(seats.begin(), seats.begin() + static_cast<std::ptrdiff_t>(i), seats[i]) !=
            seats.begin() + static_cast<std::ptrdiff_t>(i)) {
            throw ScenarioError(std::format("seat '{}' is named twice", seats[i]));
        }
        for (const AgentGroup& group : scenario.groups) {
            if ((group.name.empty() ? group.type : group.name) == seats[i]) {
                throw ScenarioError(
                    std::format("seat '{}' has the name of an agent group", seats[i]));
            }
        }
    }
    std::unique_ptr<Scorer> scorer;
    std::unique_ptr<BroadcastSink> sinks;
    if (scenario.scoring) {
        try {
            scorer = std::make_unique<Scorer>(*scenario.scoring, scenario.referencePrice);
        } catch (const std::invalid_argument& error) {
            throw ScenarioError(std::format("scoring: {}", error.what()));
        }
        sinks = std::make_unique<BroadcastSink>();
        sinks->add(*scorer);
        if (sink != nullptr) {
            sinks->add(*sink);
        }
        sink = sinks.get();
    }
    SessionMarket market{.scorer = std::move(scorer),
                         .sinks = std::move(sinks),
                         .run = ScenarioRun{scenario, registry, sink}};
    for (const std::string& name : seats) {
        auto participant = std::make_unique<Participant>();
        Seat seat{.name = name, .participant = participant.get()};
        try {
            seat.agent = market.run.addAgent(name, "participant", std::move(participant),
                                             scenario.participant);
        } catch (const std::invalid_argument& error) {
            throw ScenarioError(std::format("participant: {}", error.what()));
        }
        if (market.scorer) {
            market.scorer->addSeat(seat.agent, scenario.participant.account.initialCash,
                                   scenario.participant.account.initialPosition);
        }
        market.seats.push_back(std::move(seat));
    }
    return market;
}

RunResult SessionMarket::result() {
    RunResult result = run.result();
    if (scorer) {
        for (const Seat& seat : seats) {
            result.scores.push_back(
                {.seat = seat.name,
                 .score = scorer->score(seat.agent, run.simulation().now(), result.finalValue)});
        }
    }
    return result;
}

void writeSession(std::ostream& out, const Session& session) {
    out << "# A crowdbook session. Replay it with: crowdbook replay <this file>\n";
    out << std::format("session_version = {}\n", kSessionVersion);
    if (!session.recordedBy.empty()) {
        out << std::format("recorded_by = {}\n", tomlString("crowdbook " + session.recordedBy));
    }
    out << std::format("seed = {}\n", session.seed);
    if (session.duration) {
        out << std::format("duration_ns = {}\n", *session.duration);
    }
    out << std::format("end_ns = {}\nseats = [", session.end);
    for (std::size_t i = 0; i < session.seats.size(); ++i) {
        out << (i > 0 ? ", " : "") << tomlString(session.seats[i]);
    }
    out << std::format("]\nscenario = {}\n", tomlString(session.scenario));
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
    const std::int64_t version = reader.integer(reader.required(root, "session_version"),
                                                "session_version", 1, kSessionVersion);

    Session session{
        .scenario = reader.text(reader.required(root, "scenario"), "scenario"),
        .seed = static_cast<std::uint64_t>(
            reader.integer(reader.required(root, "seed"), "seed", 0, kMaxInt)),
        .end = reader.integer(reader.required(root, "end_ns"), "end_ns", 0, kMaxInt)};
    static_cast<void>(parseScenario(session.scenario, std::format("{} (its scenario)", source)));
    if (const toml::node* node = root.get("duration_ns")) {
        session.duration = reader.integer(*node, "duration_ns", 0, kMaxInt);
    }
    session.recordedBy.clear();
    if (const toml::node* node = root.get("recorded_by")) {
        const std::string recorded = reader.text(*node, "recorded_by");
        session.recordedBy =
            recorded.starts_with("crowdbook ") ? recorded.substr(10) : recorded;
    }
    if (version > 1) {
        const toml::node& node = reader.required(root, "seats");
        const auto* list = node.as_array();
        if (list == nullptr || list->empty()) {
            reader.fail(node, "'seats' must be a list of one or more seat names");
        }
        session.seats.clear();
        for (const toml::node& element : *list) {
            std::string seat = reader.text(element, "seats");
            if (!protocol::validSeat(seat)) {
                reader.fail(element, "a seat name has 1 to 32 letters, digits, '-' or '_'");
            }
            if (std::ranges::find(session.seats, seat) != session.seats.end()) {
                reader.fail(element, std::format("seat '{}' is named twice", seat));
            }
            session.seats.push_back(std::move(seat));
        }
    } else if (const toml::node* node = root.get("seats")) {
        reader.fail(*node, "a version 1 session has no 'seats'");
    }

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
            const SessionAction& action = session.actions.emplace_back(
                readAction(reader, *table, version, session.seats.size()));
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

Timestamp sessionEnd(const Scenario& scenario) noexcept {
    if (!scenario.tradingDay) {
        return scenario.duration;
    }
    const Latency& latency = scenario.participant.latency;
    return scenario.duration + latency.fromExchange + latency.jitter;
}

Scenario sessionScenario(const Session& session) {
    Scenario scenario = parseScenario(session.scenario, "the session's scenario");
    scenario.seed = session.seed;
    if (session.duration) {
        scenario.duration = *session.duration;
    }
    return scenario;
}

RewoundSession rewindSession(const Session& session, Timestamp at,
                             const AgentRegistry& registry, EventSink* sink) {
    if (at < 0 || at > session.end) {
        throw ScenarioError(std::format("the session runs from 0 to {} ns; it cannot be "
                                        "rewound to {} ns",
                                        session.end, at));
    }
    const Scenario scenario = sessionScenario(session);
    RewoundSession rewound{.market = openSession(scenario, registry, sink, session.seats),
                           .record = {.scenario = session.scenario,
                                      .seed = session.seed,
                                      .seats = session.seats,
                                      .duration = session.duration}};
    for (const SessionAction& action : session.actions) {
        if (action.time > at) {
            break;
        }
        rewound.market.run.runUntil(action.time);
        static_cast<void>(perform(rewound.market.run.simulation(),
                                  rewound.market.seats.at(action.seat).agent, action));
        rewound.record.actions.push_back(action);
    }
    rewound.market.run.runUntil(at);
    return rewound;
}

RunResult replaySession(const Session& session, const AgentRegistry& registry, EventSink* sink,
                        const std::vector<std::uint32_t>& without) {
    const Scenario scenario = sessionScenario(session);
    SessionMarket market = openSession(scenario, registry, sink, session.seats);
    for (const SessionAction& action : session.actions) {
        if (std::ranges::find(without, action.seat) != without.end()) {
            continue;
        }
        market.run.runUntil(action.time);
        try {
            static_cast<void>(
                perform(market.run.simulation(), market.seats.at(action.seat).agent, action));
        } catch (const std::logic_error& error) {
            if (session.recordedBy.empty() || session.recordedBy == version()) {
                throw;
            }
            throw std::logic_error(std::format(
                "{}; the session was recorded by crowdbook {}, and this is {}, whose markets "
                "may come out differently",
                error.what(), session.recordedBy, version()));
        }
    }
    market.run.runUntil(session.end);
    return market.result();
}

} // namespace crowdbook

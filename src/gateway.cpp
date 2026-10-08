#include "crowdbook/gateway.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

#include "crowdbook/live.hpp"
#include "crowdbook/protocol.hpp"

namespace crowdbook {

namespace {

constexpr std::int64_t kMaxMessagesPerSecond = 1'000'000;

// One seat's side of the translation between the client's order ids and the market's.
struct SeatState {
    std::optional<ConnectionId> connection{};
    bool stopped = false; // by the loss limit: no more orders
    // The client's ids of the seat's live orders, as the participant's ledger has them.
    std::unordered_map<ClientOrderId, ClientOrderId> wireToInternal{};
    // Kept a little longer than the above: until the order is done and every cancel or modify
    // sent for it has been answered, since an answer can arrive after the order is done.
    std::unordered_map<ClientOrderId, ClientOrderId> internalToWire{};
    std::unordered_map<ClientOrderId, std::int64_t> unanswered{};
};

// The client's id for the order a request names, and what kind of request it is.
std::pair<ClientOrderId, RequestKind> orderOf(const protocol::ClientMessage& message) {
    return std::visit(
        [](const auto& body) -> std::pair<ClientOrderId, RequestKind> {
            using Body = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<Body, NewOrder>) {
                return {body.clientOrderId, RequestKind::New};
            } else if constexpr (std::is_same_v<Body, ModifyOrder>) {
                return {body.clientOrderId, RequestKind::Modify};
            } else if constexpr (std::is_same_v<Body, CancelOrder>) {
                return {body.clientOrderId, RequestKind::Cancel};
            } else {
                return {0, RequestKind::New};
            }
        },
        message);
}

struct Connection {
    std::int64_t connectedAt = 0;
    std::optional<std::size_t> seat{};
    std::string input{};         // the start of a line not yet complete
    bool skippingLine = false;   // discarding the rest of a line that is too long
    std::string output{};        // bytes still to send, from outputStart on
    std::size_t outputStart = 0;
    bool closing = false;        // close once the output is sent
    bool dropped = false;        // close at once: too far behind
    std::int64_t allowance = 0;  // for the rate limit, in message-nanoseconds
    std::int64_t refilledAt = 0;
    std::int64_t lastSentAt = 0;

    [[nodiscard]] std::size_t pending() const noexcept { return output.size() - outputStart; }
};

} // namespace

struct Gateway::State {
    Scenario scenario;
    GatewayOptions options;
    SessionMarket market;
    Session record;
    std::vector<SeatState> seats;
    std::map<ConnectionId, Connection> connections;
    ConnectionId nextConnection = 1;
    std::optional<Pacer> pacer;
    bool finished = false;
    std::int64_t wall = 0; // the latest wall-clock time the caller has given

    State(const Scenario& scenarioIn, std::string scenarioText, const AgentRegistry& registry,
          GatewayOptions optionsIn, EventSink* sink)
        : scenario(scenarioIn), options(std::move(optionsIn)),
          market(openSession(scenario, registry, sink, options.seats)),
          record{.scenario = std::move(scenarioText),
                 .seed = scenario.seed,
                 .seats = options.seats,
                 .duration = scenario.duration},
          seats(options.seats.size()) {
        for (std::size_t i = 0; i < market.seats.size(); ++i) {
            market.seats[i].participant->setListener(
                [this, i](AgentContext& context, const Event& event) {
                    relay(i, context.now(), event);
                });
        }
        if (options.rewind) {
            replayUntil(*options.rewind, options.rewindAt);
        }
    }

    // Performs the session's actions up to `at` again and runs the market there. The orders keep
    // the ids they have inside the market as the clients' ids, so a client claiming the seat sees
    // them in its welcome and can cancel them.
    void replayUntil(const Session& session, Timestamp at) {
        for (const SessionAction& action : session.actions) {
            if (action.time > at) {
                break;
            }
            market.run.runUntil(action.time);
            SessionAction copy = action;
            const ClientOrderId id = perform(simulation(), market.seats[action.seat].agent, copy);
            record.actions.push_back(copy);
            SeatState& state = seats[action.seat];
            if (std::holds_alternative<NewOrder>(action.request)) {
                state.wireToInternal.emplace(id, id);
                state.internalToWire.emplace(id, id);
                state.unanswered.emplace(id, 0);
            } else {
                ++state.unanswered.at(id);
            }
        }
        market.run.runUntil(at);
    }

    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;

    ~State() {
        for (const Seat& seat : market.seats) {
            seat.participant->setListener({});
        }
    }

    Simulation& simulation() { return market.run.simulation(); }

    // Output.

    void send(Connection& connection, const protocol::ServerMessage& message,
              std::int64_t wallNow) {
        if (connection.closing || connection.dropped) {
            return;
        }
        connection.output += protocol::encode(message);
        connection.lastSentAt = wallNow;
        if (connection.pending() > options.maxPendingOutput) {
            // Too far behind to catch up: nothing more is sent, and the connection is closed.
            connection.dropped = true;
            connection.output.clear();
            connection.outputStart = 0;
        }
    }

    void fail(Connection& connection, std::string message, std::int64_t wallNow) {
        send(connection, protocol::Error{.message = std::move(message), .fatal = true}, wallNow);
        connection.closing = true;
    }

    void complain(Connection& connection, std::string message, std::int64_t wallNow) {
        send(connection, protocol::Error{.message = std::move(message), .fatal = false}, wallNow);
    }

    Connection* seatConnection(std::size_t seat) {
        const std::optional<ConnectionId>& id = seats[seat].connection;
        return id ? &connections.at(*id) : nullptr;
    }

    // Order events and market data reaching a seat's participant, passed on with the client's
    // ids. Called from inside the simulation, so it only queues output and updates the ids.
    void relay(std::size_t seat, Timestamp time, Event event) {
        SeatState& state = seats[seat];
        std::optional<ClientOrderId> internal;
        std::visit(
            [&](auto& body) {
                if constexpr (requires { body.clientOrderId; }) {
                    internal = body.clientOrderId;
                    body.clientOrderId = state.internalToWire.at(body.clientOrderId);
                    body.agent = 0;
                }
            },
            event);
        if (internal) {
            noteAnswer(seat, *internal, event);
        }
        if (Connection* connection = seatConnection(seat)) {
            send(*connection, protocol::MarketMessage{.time = time, .event = event}, wall);
        }
    }

    // Counts answers to cancels and modifies, and lets go of the ids of an order that is done.
    void noteAnswer(std::size_t seat, ClientOrderId internal, const Event& event) {
        SeatState& state = seats[seat];
        const auto* rejected = std::get_if<OrderRejected>(&event);
        const auto* cancelled = std::get_if<OrderCancelled>(&event);
        const bool answer =
            (rejected != nullptr && rejected->request != RequestKind::New) ||
            (cancelled != nullptr && cancelled->reason == CancelReason::Requested) ||
            std::holds_alternative<OrderModified>(event);
        if (answer) {
            --state.unanswered.at(internal);
        }
        if (simulation().ledger(market.seats[seat].agent).find(internal) != nullptr) {
            return;
        }
        const ClientOrderId wire = state.internalToWire.at(internal);
        if (const auto live = state.wireToInternal.find(wire);
            live != state.wireToInternal.end() && live->second == internal) {
            state.wireToInternal.erase(live);
        }
        if (state.unanswered.at(internal) == 0) {
            state.unanswered.erase(internal);
            state.internalToWire.erase(internal);
        }
    }

    // Requests.

    // Sends a request from the seat now and records it. Returns the client order id it was sent
    // with inside the market.
    ClientOrderId act(std::size_t seat, Request request) {
        Simulation& sim = simulation();
        SessionAction action{.time = sim.now(),
                             .seat = static_cast<std::uint32_t>(seat),
                             .request = std::move(request)};
        const ClientOrderId id = perform(sim, market.seats[seat].agent, action);
        if (auto* order = std::get_if<NewOrder>(&action.request)) {
            order->clientOrderId = id;
        }
        record.actions.push_back(std::move(action));
        return id;
    }

    void rejectAtOnce(Connection& connection, ClientOrderId wire, RequestKind request,
                      RejectReason reason, std::int64_t wallNow) {
        send(connection,
             protocol::MarketMessage{.time = simulation().now(),
                                     .event = OrderRejected{.clientOrderId = wire,
                                                            .request = request,
                                                            .reason = reason}},
             wallNow);
    }

    void trade(Connection& connection, std::size_t seat, const protocol::ClientMessage& message,
               std::int64_t wallNow) {
        SeatState& state = seats[seat];
        if (state.stopped && !std::holds_alternative<CancelOrder>(message)) {
            const auto [wire, kind] = orderOf(message);
            rejectAtOnce(connection, wire, kind, RejectReason::LossLimit, wallNow);
            return;
        }
        if (const auto* order = std::get_if<NewOrder>(&message)) {
            if (state.wireToInternal.contains(order->clientOrderId)) {
                rejectAtOnce(connection, order->clientOrderId, RequestKind::New,
                             RejectReason::DuplicateClientOrderId, wallNow);
                return;
            }
            NewOrder inside = *order;
            inside.clientOrderId = 0;
            const ClientOrderId internal = act(seat, inside);
            state.wireToInternal.emplace(order->clientOrderId, internal);
            state.internalToWire.emplace(internal, order->clientOrderId);
            state.unanswered.emplace(internal, 0);
            return;
        }
        const ClientOrderId wire = std::visit(
            [](const auto& body) -> ClientOrderId {
                if constexpr (requires { body.clientOrderId; }) {
                    return body.clientOrderId;
                } else {
                    return 0;
                }
            },
            message);
        const RequestKind kind = std::holds_alternative<CancelOrder>(message)
                                     ? RequestKind::Cancel
                                     : RequestKind::Modify;
        const auto live = state.wireToInternal.find(wire);
        if (live == state.wireToInternal.end()) {
            rejectAtOnce(connection, wire, kind, RejectReason::UnknownOrderId, wallNow);
            return;
        }
        const ClientOrderId internal = live->second;
        if (kind == RequestKind::Cancel) {
            act(seat, CancelOrder{.clientOrderId = internal});
        } else {
            const auto& modify = std::get<ModifyOrder>(message);
            act(seat, ModifyOrder{.clientOrderId = internal,
                                  .price = modify.price,
                                  .quantity = modify.quantity});
        }
        ++state.unanswered.at(internal);
    }

    // Cancels every open order of the seat whose cancel has not been asked for already.
    void cancelAll(std::size_t seat) {
        std::vector<ClientOrderId> open;
        for (const auto& [internal, own] :
             simulation().ledger(market.seats[seat].agent).orders()) {
            if (!own.cancelRequested) {
                open.push_back(internal);
            }
        }
        for (const ClientOrderId internal : open) {
            act(seat, CancelOrder{.clientOrderId = internal});
            ++seats[seat].unanswered.at(internal);
        }
    }

    // Seats.

    protocol::Welcome welcome(std::size_t seat) {
        const Simulation& sim = simulation();
        const Ledger& ledger = sim.ledger(market.seats[seat].agent);
        const AccountConfig& account = scenario.participant.account;
        protocol::Welcome message{
            .seat = market.seats[seat].name,
            .started = pacer.has_value(),
            .time = sim.now(),
            .duration = scenario.duration,
            .referencePrice = scenario.referencePrice,
            .depthLevels = scenario.exchange.depthLevels,
            .makerFee = scenario.exchange.makerFee,
            .takerFee = scenario.exchange.takerFee,
            .auctionFee = scenario.exchange.auctionFee,
            .phase = sim.marketSeenBy(market.seats[seat].agent).phase,
            .latency = scenario.participant.latency,
            .account = {.initialCash = account.initialCash,
                        .initialPosition = account.initialPosition,
                        .cash = ledger.cash(),
                        .position = ledger.position(),
                        .fees = ledger.fees(),
                        .maxPosition = account.maxPosition,
                        .maxOrderQuantity = account.maxOrderQuantity},
            .scoring = scenario.scoring};
        if (scenario.challenge) {
            message.challenge = protocol::ChallengeInfo{.name = scenario.challenge->name,
                                                        .briefing = scenario.challenge->briefing};
        }
        for (const auto& [internal, own] : ledger.orders()) {
            OwnOrder order = own;
            order.clientOrderId = seats[seat].internalToWire.at(internal);
            message.orders.push_back(order);
        }
        return message;
    }

    void claim(Connection& connection, ConnectionId id, const protocol::Hello& hello,
               std::int64_t wallNow) {
        if (connection.seat) {
            complain(connection,
                     std::format("this connection already has the seat '{}'",
                                 market.seats[*connection.seat].name),
                     wallNow);
            return;
        }
        if (hello.protocol != protocol::kVersion) {
            fail(connection,
                 std::format("this server speaks protocol {}, not {}", protocol::kVersion,
                             hello.protocol),
                 wallNow);
            return;
        }
        const auto named = std::ranges::find(options.seats, hello.seat);
        if (named == options.seats.end()) {
            fail(connection, std::format("there is no seat '{}'", hello.seat), wallNow);
            return;
        }
        if (!options.tokens.empty() && options.tokens.at(hello.seat) != hello.token) {
            fail(connection, std::format("wrong token for the seat '{}'", hello.seat), wallNow);
            return;
        }
        const auto seat = static_cast<std::size_t>(named - options.seats.begin());
        if (seats[seat].connection) {
            fail(connection, std::format("the seat '{}' is taken", hello.seat), wallNow);
            return;
        }
        if (finished) {
            fail(connection, "the session is over", wallNow);
            return;
        }
        connection.seat = seat;
        seats[seat].connection = id;
        send(connection, welcome(seat), wallNow);
        if (!pacer && std::ranges::all_of(seats, [](const SeatState& s) {
                return s.connection.has_value();
            })) {
            pacer.emplace(wallNow, simulation().now(), options.speed);
            for (auto& [other, open] : connections) {
                if (open.seat) {
                    send(open, protocol::Start{.time = simulation().now()}, wallNow);
                }
            }
        }
    }

    // One complete line from a connection.
    void handle(Connection& connection, ConnectionId id, std::string_view line,
                std::int64_t wallNow) {
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            return;
        }
        if (!withinRate(connection, wallNow)) {
            // An order request over the limit is rejected by its id, so that the client's books
            // stay right; anything else gets an error.
            std::optional<protocol::ClientMessage> dropped;
            try {
                dropped = protocol::decodeClient(line);
            } catch (const protocol::ProtocolError&) {
            }
            if (dropped && connection.seat && !std::holds_alternative<protocol::Hello>(*dropped)) {
                const auto [wire, kind] = orderOf(*dropped);
                rejectAtOnce(connection, wire, kind, RejectReason::RateLimit, wallNow);
            } else {
                complain(connection,
                         std::format("over the limit of {} messages a second; message dropped",
                                     options.maxMessagesPerSecond),
                         wallNow);
            }
            return;
        }
        protocol::ClientMessage message;
        try {
            message = protocol::decodeClient(line);
        } catch (const protocol::ProtocolError& error) {
            if (connection.seat) {
                complain(connection, error.what(), wallNow);
            } else {
                fail(connection, error.what(), wallNow);
            }
            return;
        }
        if (const auto* hello = std::get_if<protocol::Hello>(&message)) {
            claim(connection, id, *hello, wallNow);
            return;
        }
        if (!connection.seat) {
            fail(connection, "the first message must be a hello", wallNow);
            return;
        }
        if (finished) {
            return;
        }
        if (!pacer) {
            complain(connection, "the market has not started; message dropped", wallNow);
            return;
        }
        trade(connection, *connection.seat, message, wallNow);
    }

    // A token bucket: a full second's worth of messages at most, refilled continuously.
    bool withinRate(Connection& connection, std::int64_t wallNow) {
        const std::int64_t rate = options.maxMessagesPerSecond;
        const std::int64_t elapsed = std::min(wallNow - connection.refilledAt, kSecond);
        connection.allowance = std::min(connection.allowance + elapsed * rate, kSecond * rate);
        connection.refilledAt = wallNow;
        if (connection.allowance < kSecond) {
            return false;
        }
        connection.allowance -= kSecond;
        return true;
    }

    // Time.

    void advance(std::int64_t wallNow) {
        wall = wallNow;
        for (auto& [id, connection] : connections) {
            if (!connection.seat && !connection.closing &&
                wallNow - connection.connectedAt >= options.helloTimeout) {
                fail(connection, "no hello in time", wallNow);
            }
        }
        if (finished) {
            return;
        }
        if (pacer) {
            const Timestamp end = sessionEnd(scenario);
            const Timestamp target = std::min(pacer->simulatedAt(wallNow), end);
            if (target > simulation().now()) {
                market.run.runUntil(target);
            }
            stopLosers(wallNow);
            if (simulation().now() >= end) {
                finish(wallNow);
                return;
            }
            for (auto& [id, connection] : connections) {
                if (connection.seat && wallNow - connection.lastSentAt >= options.clockInterval) {
                    send(connection, protocol::Clock{.time = simulation().now()}, wallNow);
                }
            }
        }
    }

    // Seats whose loss reached the limit while the market ran have their orders cancelled and
    // can send no more.
    void stopLosers(std::int64_t wallNow) {
        if (!market.scorer) {
            return;
        }
        for (std::size_t seat = 0; seat < seats.size(); ++seat) {
            const std::optional<Timestamp> at = market.scorer->stoppedAt(market.seats[seat].agent);
            if (!at || seats[seat].stopped) {
                continue;
            }
            seats[seat].stopped = true;
            cancelAll(seat);
            if (Connection* connection = seatConnection(seat)) {
                complain(*connection,
                         std::format("stopped at {} ns: the loss reached the limit of {}; open "
                                     "orders are cancelled and no more are taken",
                                     *at, scenario.scoring->maxLoss),
                         wallNow);
            }
        }
    }

    void finish(std::int64_t wallNow) {
        finished = true;
        record.end = simulation().now();
        const RunResult result = market.result();
        for (auto& [id, connection] : connections) {
            if (connection.seat) {
                const std::string& name = market.seats[*connection.seat].name;
                const auto group = std::ranges::find(result.groups, name, &GroupResult::name);
                protocol::End end{.time = simulation().now(),
                                  .cash = group->cash,
                                  .position = group->position,
                                  .fees = group->fees,
                                  .pnl = group->pnl};
                if (const auto score = std::ranges::find(result.scores, name, &SeatScore::seat);
                    score != result.scores.end()) {
                    end.score = score->score;
                }
                send(connection, end, wallNow);
            }
            connection.closing = true;
        }
    }
};

Gateway::Gateway(const Scenario& scenario, std::string scenarioText,
                 const AgentRegistry& registry, GatewayOptions options, EventSink* sink) {
    if (options.rewind) {
        const Session& session = *options.rewind;
        if (options.rewindAt < 0 || options.rewindAt > session.end ||
            options.rewindAt > scenario.duration) {
            throw std::invalid_argument(std::format(
                "a session that ran {} ns cannot be rewound to {} ns", session.end,
                options.rewindAt));
        }
        options.seats = session.seats;
        scenarioText = session.scenario;
    }
    if (!(options.speed > 0.0) || !std::isfinite(options.speed)) {
        throw std::invalid_argument("the speed must be positive and finite");
    }
    for (const std::string& seat : options.seats) {
        if (!protocol::validSeat(seat)) {
            throw std::invalid_argument(
                std::format("'{}' is not a seat name: 1 to {} letters, digits, '-' or '_'", seat,
                            protocol::kMaxSeatLength));
        }
    }
    if (!options.tokens.empty()) {
        for (const std::string& seat : options.seats) {
            if (!options.tokens.contains(seat)) {
                throw std::invalid_argument(std::format("the seat '{}' has no token", seat));
            }
        }
        if (options.tokens.size() != options.seats.size()) {
            throw std::invalid_argument("there are tokens for seats the market does not have");
        }
    }
    if (options.maxMessagesPerSecond < 1 || options.maxMessagesPerSecond > kMaxMessagesPerSecond) {
        throw std::invalid_argument(std::format("the message rate limit must be from 1 to {}",
                                                kMaxMessagesPerSecond));
    }
    if (options.maxPendingOutput == 0 || options.helloTimeout <= 0 ||
        options.clockInterval <= 0) {
        throw std::invalid_argument("output limits, timeouts and intervals must be positive");
    }
    state_ = std::make_unique<State>(scenario, std::move(scenarioText), registry,
                                     std::move(options), sink);
}

Gateway::~Gateway() = default;

ConnectionId Gateway::connect(std::int64_t wallNow) {
    const ConnectionId id = state_->nextConnection++;
    state_->connections.emplace(id, Connection{.connectedAt = wallNow,
                                               .allowance = kSecond *
                                                            state_->options.maxMessagesPerSecond,
                                               .refilledAt = wallNow,
                                               .lastSentAt = wallNow});
    return id;
}

void Gateway::receive(ConnectionId id, std::string_view bytes, std::int64_t wallNow) {
    const auto found = state_->connections.find(id);
    if (found == state_->connections.end()) {
        return;
    }
    state_->advance(wallNow);
    Connection& connection = found->second;
    while (!bytes.empty() && !connection.closing && !connection.dropped) {
        const std::size_t newline = bytes.find('\n');
        const std::string_view piece =
            bytes.substr(0, newline == std::string_view::npos ? bytes.size() : newline);
        if (!connection.skippingLine) {
            connection.input += piece;
            if (connection.input.size() >= protocol::kMaxLineLength) {
                state_->complain(connection,
                                 std::format("a message longer than {} bytes; dropped",
                                             protocol::kMaxLineLength),
                                 wallNow);
                connection.input.clear();
                connection.skippingLine = true;
            }
        }
        if (newline == std::string_view::npos) {
            return;
        }
        bytes.remove_prefix(newline + 1);
        if (connection.skippingLine) {
            connection.skippingLine = false;
            continue;
        }
        const std::string line = std::move(connection.input);
        connection.input.clear();
        state_->handle(connection, id, line, wallNow);
    }
}

void Gateway::disconnect(ConnectionId id, std::int64_t wallNow) {
    const auto found = state_->connections.find(id);
    if (found == state_->connections.end()) {
        return;
    }
    state_->advance(wallNow);
    const std::optional<std::size_t> seat = found->second.seat;
    state_->connections.erase(found);
    if (seat) {
        state_->seats[*seat].connection.reset();
        if (state_->pacer && !state_->finished) {
            state_->cancelAll(*seat);
        }
    }
}

void Gateway::advance(std::int64_t wallNow) {
    state_->advance(wallNow);
}

void Gateway::stop(std::int64_t wallNow) {
    state_->advance(wallNow);
    if (!state_->finished) {
        state_->finish(wallNow);
    }
}

std::string_view Gateway::pendingOutput(ConnectionId id) const {
    const auto found = state_->connections.find(id);
    if (found == state_->connections.end()) {
        return {};
    }
    return std::string_view{found->second.output}.substr(found->second.outputStart);
}

void Gateway::consumeOutput(ConnectionId id, std::size_t bytes) {
    const auto found = state_->connections.find(id);
    if (found == state_->connections.end()) {
        return;
    }
    Connection& connection = found->second;
    connection.outputStart += std::min(bytes, connection.pending());
    // Sent bytes are dropped from the front once they are most of the buffer, so each byte is
    // moved at most once on average.
    if (connection.outputStart > connection.output.size() / 2) {
        connection.output.erase(0, connection.outputStart);
        connection.outputStart = 0;
    }
}

bool Gateway::shouldClose(ConnectionId id) const {
    const auto found = state_->connections.find(id);
    if (found == state_->connections.end()) {
        return true;
    }
    const Connection& connection = found->second;
    return connection.dropped || (connection.closing && connection.pending() == 0);
}

bool Gateway::started() const noexcept {
    return state_->pacer.has_value();
}

bool Gateway::finished() const noexcept {
    return state_->finished;
}

Timestamp Gateway::now() const {
    return state_->market.run.simulation().now();
}

const Session& Gateway::session() const noexcept {
    return state_->record;
}

RunResult Gateway::result() {
    return state_->market.result();
}

const SessionMarket& Gateway::market() const noexcept {
    return state_->market;
}

} // namespace crowdbook

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/event_log.hpp"
#include "crowdbook/scenario.hpp"
#include "crowdbook/session.hpp"

namespace crowdbook {

struct GatewayOptions {
    // The seats clients can claim, in the order their participants join the market.
    std::vector<std::string> seats{std::string{kParticipantGroup}};
    // Each seat's token, which a client claiming it must give; no tokens if empty. Otherwise
    // every seat needs one.
    std::map<std::string, std::string> tokens{};
    double speed = 1.0; // simulated seconds per second of wall-clock time
    // Messages each connection may send per second of wall-clock time, with bursts of as many.
    std::int64_t maxMessagesPerSecond = 500;
    // A connection with more than this many bytes waiting to be sent to it is closed.
    std::size_t maxPendingOutput = std::size_t{8} << 20;
    // How long, in wall-clock nanoseconds, a connection has to claim a seat.
    std::int64_t helloTimeout = 5 * kSecond;
    // How often, in wall-clock nanoseconds, a quiet connection is sent the market's time.
    std::int64_t clockInterval = 100 * kMillisecond;
    // A session to rewind to `rewindAt` and serve on from there. Its seats replace `seats`, and
    // its orders keep their ids inside the market as the clients' ids.
    std::optional<Session> rewind{};
    Timestamp rewindAt = 0;
};

using ConnectionId = std::uint64_t;

// Runs a scenario's market for clients that trade in it through the protocol in
// docs/protocol.md. It works on bytes and never touches a socket: whoever owns the connections
// passes in what arrives, the wall-clock time, and takes out what to send. Everything happens on
// the caller's thread.
//
// Wall-clock times are nanoseconds from any fixed origin and must never go backwards.
class Gateway {
public:
    // Builds the market with one participant per seat. `scenarioText` is kept in the recording.
    // Throws ScenarioError as openSession does, and std::invalid_argument for options that make no
    // sense: a speed that is not positive and finite, tokens for some seats but not others, or
    // limits that are not positive.
    Gateway(const Scenario& scenario, std::string scenarioText, const AgentRegistry& registry,
            GatewayOptions options, EventSink* sink = nullptr);
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;
    Gateway(Gateway&&) = delete;
    Gateway& operator=(Gateway&&) = delete;
    ~Gateway();

    // A new connection, which has helloTimeout to claim a seat.
    ConnectionId connect(std::int64_t wallNow);
    // Bytes that arrived on a connection. Each complete line is one message, acted on at the
    // market's time for `wallNow`. Bytes for a connection that is closing are ignored.
    void receive(ConnectionId connection, std::string_view bytes, std::int64_t wallNow);
    // The connection is gone. Its seat's open orders are cancelled and the seat can be claimed
    // again. Unknown connections are ignored.
    void disconnect(ConnectionId connection, std::int64_t wallNow);

    // Runs the market up to the time for `wallNow`, sends what it produced, closes connections
    // that never claimed a seat, and finishes the session when its time is up. Before every seat
    // is claimed the market stands still at 0.
    void advance(std::int64_t wallNow);
    // Ends the session now, as if its time were up.
    void stop(std::int64_t wallNow);

    // For whoever runs the gateway: how fast the market's clock runs against the wall clock, and
    // pausing it. Both take effect from `wallNow`, with the market's time carrying on from where
    // it was. setSpeed throws std::invalid_argument unless the speed is positive and finite.
    void setSpeed(double speed, std::int64_t wallNow);
    void setPaused(bool paused, std::int64_t wallNow);
    [[nodiscard]] double speed() const noexcept;
    [[nodiscard]] bool paused() const noexcept;

    // What is waiting to be sent on a connection, and how much of it was sent.
    [[nodiscard]] std::string_view pendingOutput(ConnectionId connection) const;
    void consumeOutput(ConnectionId connection, std::size_t bytes);
    // True once the connection should be closed: after a fatal error or the end of the session,
    // and once everything waiting has been sent, or at once for a connection that fell too far
    // behind. Its caller should then close it and call disconnect.
    [[nodiscard]] bool shouldClose(ConnectionId connection) const;
    // Whether what is waiting holds more than public market data and the time: the seat's own
    // order events, an error, the welcome, the start or the end. A caller that holds market data
    // back to send it in batches should still send these at once.
    [[nodiscard]] bool urgent(ConnectionId connection) const;
    // For a caller that sends market data in batches: a depth or top of book update still waiting
    // to be sent when a newer one comes is dropped, so each batch carries only the latest of each.
    // Trades, the seat's own events and everything else are all sent. Off by default.
    void conflate(bool conflating);

    // For a caller that waits on many connections: the connections given something to send, or
    // told to close, since the last call, each once, appended to `ready`. Only these need looking
    // at after a call to advance, receive or disconnect.
    void takeReady(std::vector<ConnectionId>& ready);
    // The wall-clock time by which advance should next be called, if nothing arrives first: when
    // the market's next event is due, a quiet connection is owed the time, or a connection runs
    // out of time to claim a seat. Nullopt when nothing is due until something arrives.
    [[nodiscard]] std::optional<std::int64_t> nextWake() const;

    [[nodiscard]] bool started() const noexcept;
    [[nodiscard]] bool finished() const noexcept;
    // The market's time now.
    [[nodiscard]] Timestamp now() const;
    // The session as recorded so far: the scenario, the seed, the seats and every request.
    [[nodiscard]] const Session& session() const noexcept;
    [[nodiscard]] RunResult result();
    [[nodiscard]] const SessionMarket& market() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace crowdbook

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/agents/participant.hpp"
#include "crowdbook/event_log.hpp"
#include "crowdbook/live.hpp"
#include "crowdbook/scenario.hpp"
#include "crowdbook/scoring.hpp"
#include "crowdbook/version.hpp"

namespace crowdbook {

// The name of the seat in a session with one participant, as `crowdbook play` runs, and so the
// group that participant is reported as in results.
inline constexpr std::string_view kParticipantGroup = "you";

// A place for a person or a program to trade from outside the market.
struct Seat {
    std::string name{};
    AgentId agent = 0;
    Participant* participant = nullptr; // owned by the run's simulation
};

// A scenario's market with live participants added, as a session and its replay both build it:
// one participant per seat, joining after the scenario's agents in the order the seats are named,
// each with the scenario's [participant] account and latency and reported as a group named after
// its seat.
struct SessionMarket {
    // With a [scoring] section, the scorer, which sees every request and event, and the sink
    // that passes them to it and to the caller's sink. Kept apart so their addresses never move.
    std::unique_ptr<Scorer> scorer{};
    std::unique_ptr<BroadcastSink> sinks{};
    ScenarioRun run;
    std::vector<Seat> seats{};

    // The run's results so far, with each seat's score when the scenario has scoring.
    [[nodiscard]] RunResult result();
};

// Throws ScenarioError as ScenarioRun does, and for no seats, a seat named twice or a seat with
// the name of one of the scenario's agent groups. With a [scoring] section, each seat is scored.
[[nodiscard]] SessionMarket openSession(
    const Scenario& scenario, const AgentRegistry& registry, EventSink* sink = nullptr,
    const std::vector<std::string>& seats = {std::string{kParticipantGroup}});

// A live session as recorded: the market it was played in, its seats and everything the
// participants did, which is enough to replay it exactly.
struct Session {
    std::string scenario{}; // the scenario file's text, so a session replays without the file
    std::uint64_t seed = 1; // the seed the session ran with, which overrides the scenario's
    Timestamp end = 0;      // the simulated time the session stopped at
    std::vector<std::string> seats{std::string{kParticipantGroup}};
    std::vector<SessionAction> actions{}; // each names its seat by index
    // The crowdbook that recorded it: this one for a new session, empty when a file does not say.
    std::string recordedBy{version()};
    // The scenario's length as the session ran it, which a trading day's schedule depends on,
    // when the session was started with a duration of its own; otherwise the scenario's.
    std::optional<Duration> duration{};

    friend bool operator==(const Session&, const Session&) = default;
};

// When a live session of the scenario ends: at its duration, or with a trading day once the
// close and the closing auction's fills, made at the duration, have reached the participants.
// The market is closed by then, so nothing more trades.
[[nodiscard]] Timestamp sessionEnd(const Scenario& scenario) noexcept;

// The scenario a session ran: its text, with the session's seed and duration.
[[nodiscard]] Scenario sessionScenario(const Session& session);

// Writes a session as TOML; docs/scenarios.md describes the format.
void writeSession(std::ostream& out, const Session& session);

// Parses a session file, of the current version or an earlier one. Throws ScenarioError, with the
// line, for anything malformed, including a scenario that does not parse, actions out of time
// order and an action from a seat the session does not have.
[[nodiscard]] Session parseSession(std::string_view text, std::string_view source = "session");

// Reads and parses a session file. Throws ScenarioError if it cannot be read or parsed.
[[nodiscard]] Session loadSession(const std::filesystem::path& path);

// A session rewound to `at`: its market built with openSession, every action up to and including
// `at` performed again, and the market run to `at`, ready to be played on from there. The
// actions performed are in the returned recording, which a session played on from here extends.
// Throws as replaySession does, and ScenarioError for a moment past the session's end.
struct RewoundSession {
    SessionMarket market;
    Session record;
};
[[nodiscard]] RewoundSession rewindSession(const Session& session, Timestamp at,
                                           const AgentRegistry& registry,
                                           EventSink* sink = nullptr);

// Replays a session: builds its market with openSession, performs every action at its time and
// runs to the session's end. Every request and event goes to `sink`, if there is one, so the
// event log comes out exactly as it did during the session, and the scores are computed again.
// The actions of the seats listed in `without`, by index, are left out: their participants are
// still there but send nothing, which replays the market as it would have been without them.
// Throws std::logic_error if the replay diverges from the recording, which leaving seats out
// can make it do only through the client order ids of the seats that remain, and never does.
[[nodiscard]] RunResult replaySession(const Session& session, const AgentRegistry& registry,
                                      EventSink* sink = nullptr,
                                      const std::vector<std::uint32_t>& without = {});

} // namespace crowdbook

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
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

    friend bool operator==(const Session&, const Session&) = default;
};

// Writes a session as TOML; docs/scenarios.md describes the format.
void writeSession(std::ostream& out, const Session& session);

// Parses a session file, of the current version or an earlier one. Throws ScenarioError, with the
// line, for anything malformed, including a scenario that does not parse, actions out of time
// order and an action from a seat the session does not have.
[[nodiscard]] Session parseSession(std::string_view text, std::string_view source = "session");

// Reads and parses a session file. Throws ScenarioError if it cannot be read or parsed.
[[nodiscard]] Session loadSession(const std::filesystem::path& path);

// Replays a session: builds its market with openSession, performs every action at its time and
// runs to the session's end. Every request and event goes to `sink`, if there is one, so the
// event log comes out exactly as it did during the session, and the scores are computed again.
// Throws std::logic_error if the replay diverges from the recording.
[[nodiscard]] RunResult replaySession(const Session& session, const AgentRegistry& registry,
                                      EventSink* sink = nullptr);

} // namespace crowdbook

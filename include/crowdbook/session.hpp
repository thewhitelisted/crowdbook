#pragma once

#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/agents/participant.hpp"
#include "crowdbook/event_log.hpp"
#include "crowdbook/live.hpp"
#include "crowdbook/scenario.hpp"

namespace crowdbook {

// The group the live participant is reported as in results.
inline constexpr std::string_view kParticipantGroup = "you";

// A scenario's market with a live participant added, as a session and its replay both build it:
// the participant joins last, with the scenario's [participant] account and latency.
struct SessionMarket {
    ScenarioRun run;
    AgentId participant = 0;
    Participant* agent = nullptr; // owned by the run's simulation
};

// Throws ScenarioError as ScenarioRun does.
[[nodiscard]] SessionMarket openSession(const Scenario& scenario, const AgentRegistry& registry,
                                        EventSink* sink = nullptr);

// A live session as recorded: the market it was played in and everything the participant did,
// which is enough to replay it exactly.
struct Session {
    std::string scenario{}; // the scenario file's text, so a session replays without the file
    std::uint64_t seed = 1; // the seed the session ran with, which overrides the scenario's
    Timestamp end = 0;      // the simulated time the session stopped at
    std::vector<SessionAction> actions{};

    friend bool operator==(const Session&, const Session&) = default;
};

// Writes a session as TOML; docs/scenarios.md describes the format.
void writeSession(std::ostream& out, const Session& session);

// Parses a session file. Throws ScenarioError, with the line, for anything malformed, including
// a scenario that does not parse and actions out of time order.
[[nodiscard]] Session parseSession(std::string_view text, std::string_view source = "session");

// Reads and parses a session file. Throws ScenarioError if it cannot be read or parsed.
[[nodiscard]] Session loadSession(const std::filesystem::path& path);

// Replays a session: builds its market with openSession, performs every action at its time and
// runs to the session's end. Every request and event goes to `sink`, if there is one, so the
// event log comes out exactly as it did during the session. Throws std::logic_error if the replay
// diverges from the recording.
[[nodiscard]] RunResult replaySession(const Session& session, const AgentRegistry& registry,
                                      EventSink* sink = nullptr);

} // namespace crowdbook

#pragma once

#include <filesystem>
#include <string_view>

#include "crowdbook/scenario.hpp"

namespace crowdbook {

// Parses a scenario written in TOML; docs/scenarios.md describes the format. `source` names the
// text in error messages. Throws ScenarioError, with the line, for syntax errors, unknown or
// misspelled keys, and values of the wrong type or out of range. Agent parameters are checked
// later, when runScenario creates the agents.
[[nodiscard]] Scenario parseScenario(std::string_view text, std::string_view source = "scenario");

// Reads and parses a scenario file. Throws ScenarioError if it cannot be read or parsed.
[[nodiscard]] Scenario loadScenario(const std::filesystem::path& path);

} // namespace crowdbook

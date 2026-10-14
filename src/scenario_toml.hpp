#pragma once

#include <string_view>

#include <toml++/toml.hpp>

#include "crowdbook/scenario.hpp"

namespace crowdbook::detail {

// Reads a scenario from a parsed TOML document, as parseScenario does from text, with the same
// checks; errors name `source` and the line, where the document has one.
[[nodiscard]] Scenario readScenario(const toml::table& root, std::string_view source);

} // namespace crowdbook::detail

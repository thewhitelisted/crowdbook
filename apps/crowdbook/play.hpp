#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "crowdbook/types.hpp"
#include "output.hpp"

namespace crowdbook {

struct PlayOptions {
    std::string scenarioPath{};
    std::optional<std::uint64_t> seed{};
    std::optional<Duration> duration{};
    double speed = 1.0;
    std::optional<std::string> recordPath{}; // where to write the session, for crowdbook replay
    OutputOptions outputs{};                 // of which play writes only the event log
};

// Runs the scenario in real time on a trading screen in the terminal, with the person at the
// keyboard trading as its participant, then prints the results. Returns the exit code.
int play(const PlayOptions& options);

} // namespace crowdbook

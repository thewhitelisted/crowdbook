#pragma once

#include "live_session.hpp"

namespace crowdbook {

struct PlayOptions {
    LiveOptions live{}; // the path is a scenario, or with rewindAt a session with one seat
    double speed = 1.0;
};

// Runs the scenario in real time on a trading screen in the terminal, with the person at the
// keyboard trading as its participant, then prints the results. Returns the exit code.
int play(const PlayOptions& options);

} // namespace crowdbook

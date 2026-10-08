#pragma once

#include <cstdint>
#include <fstream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include "crowdbook/gateway.hpp"
#include "crowdbook/scenario.hpp"
#include "crowdbook/session.hpp"
#include "output.hpp"

namespace crowdbook {

// What `play` and `serve` share: the market they run, from a scenario file or a session rewound
// to a moment, the files they write, and what they print at the end. Both run the market through
// a gateway; `play` is a gateway with one client in the same process.
struct LiveOptions {
    std::string path{}; // a scenario, or with rewindAt a session
    std::optional<std::uint64_t> seed{};
    std::optional<Duration> duration{};
    std::optional<Timestamp> rewindAt{};
    std::optional<std::string> recordPath{}; // where to write the session, for crowdbook replay
    std::optional<std::string> reportPath{}; // where to write the session report, as JSON
    OutputOptions outputs{};                 // of which only the log and the JSON results apply
};

class LiveSession {
public:
    // Loads the market and opens every file it will write, so that a path that cannot be written
    // fails before anyone trades. `options` gets the session to rewind, if there is one, and its
    // seats. Throws as loading and parsing do, and std::runtime_error for a file it cannot open.
    LiveSession(const LiveOptions& live, GatewayOptions options);

    [[nodiscard]] Gateway& gateway() noexcept { return *gateway_; }
    [[nodiscard]] const Scenario& scenario() const noexcept { return scenario_; }
    [[nodiscard]] const std::vector<std::string>& seats() const noexcept { return seats_; }

    // Prints what happened, "played" or "served", and the results, and writes the outputs, the
    // recording and the report that were asked for.
    void finish(std::ostream& out, std::string_view verb);

private:
    LiveOptions live_;
    Scenario scenario_;
    std::vector<std::string> seats_;
    std::optional<Outputs> outputs_;
    std::ofstream record_;
    std::ofstream report_;
    std::optional<Gateway> gateway_;
};

// A file's whole text. Throws std::runtime_error if it cannot be read.
[[nodiscard]] std::string readFile(const std::string& path);

} // namespace crowdbook

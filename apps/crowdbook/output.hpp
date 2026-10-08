#pragma once

#include <fstream>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "crowdbook/event_log.hpp"
#include "crowdbook/report.hpp"
#include "crowdbook/scenario.hpp"
#include "crowdbook/session.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// A mistake in the command line; main prints the message followed by the usage.
class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The files a run can write besides its summary.
struct OutputOptions {
    std::optional<std::string> logPath{};
    std::vector<std::string> logKinds{}; // empty logs every kind
    std::optional<std::string> jsonPath{};
    std::optional<std::string> pricesPath{};
    std::optional<std::string> depthPath{};
    Duration sampleInterval = kSecond; // for the price and depth samples
};

// The files a run writes. All of them are opened at the start, so that a path that cannot be
// written fails before the run. Throws std::runtime_error for a file that cannot be opened or
// written in full, or depth samples asked of a market without a depth feed.
class Outputs {
public:
    Outputs(const OutputOptions& options, const Scenario& scenario);
    Outputs(const Outputs&) = delete;
    Outputs& operator=(const Outputs&) = delete;

    // The sink that every request and event of the run should go to.
    [[nodiscard]] EventSink* sink() noexcept { return &sinks_; }
    // Writes the samples still due when the run ends at `end`.
    void finish(Timestamp end);
    // Writes the JSON results, if asked for, and says where each file went.
    void report(std::ostream& out, const Scenario& scenario, const RunResult& result);

private:
    OutputOptions options_;
    std::vector<char> logBuffer_; // declared before the file, so it outlives it
    std::ofstream logFile_;
    std::ofstream pricesFile_;
    std::ofstream depthFile_;
    std::ofstream jsonFile_;
    std::optional<CsvEventLog> log_;
    std::optional<PriceSampler> prices_;
    std::optional<DepthSampler> depth_;
    BroadcastSink sinks_;
};

// The results table: each group's position, cash and PnL, with fee columns when the exchange
// charges fees.
void printResults(std::ostream& out, const Scenario& scenario, const RunResult& result);

// A session report in brief: for each seat, its score, who it traded with and what they knew,
// and what the market would have done without it.
void printReport(std::ostream& out, const SessionReport& report);

// How many threads work that can be spread out, such as a report's replays, should use.
[[nodiscard]] unsigned everyCore();

// Builds the session's report, prints it in brief and writes it to `file`, opened at `path`.
// Throws std::runtime_error if the file cannot be written in full.
void writeReport(std::ofstream& file, const std::string& path, const Session& session);

// A challenge's name and briefing, and how it is scored.
void printBriefing(std::ostream& out, const Scenario& scenario);

// Points as signed tick-lots, exactly: 12345 is "+12.345" and -250 is "-0.25".
[[nodiscard]] std::string formatPoints(Points points);

// A duration in the largest whole unit, such as "60s" or "250ms", or in seconds to the millisecond
// when it is longer than a second and not a whole number of milliseconds: "58.857s".
[[nodiscard]] std::string formatDuration(Duration duration);

} // namespace crowdbook

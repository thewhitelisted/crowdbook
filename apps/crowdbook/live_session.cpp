#include "live_session.hpp"

#include <format>
#include <sstream>
#include <stdexcept>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/scenario_file.hpp"

namespace crowdbook {

namespace {

std::ofstream openForWriting(const std::optional<std::string>& path) {
    std::ofstream file;
    if (path) {
        file.open(*path);
        if (!file) {
            throw std::runtime_error(std::format("cannot write '{}'", *path));
        }
    }
    return file;
}

} // namespace

std::string readFile(const std::string& path) {
    std::ifstream file{path};
    if (!file) {
        throw std::runtime_error(std::format("cannot read '{}'", path));
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

LiveSession::LiveSession(const LiveOptions& live, GatewayOptions options) : live_(live) {
    std::optional<Session> from;
    std::string text;
    if (live.rewindAt) {
        from = loadSession(live.path);
        text = from->scenario;
        scenario_ = sessionScenario(*from);
        options.rewind = from;
        options.rewindAt = *live.rewindAt;
        seats_ = from->seats;
    } else {
        text = readFile(live.path);
        scenario_ = parseScenario(text, live.path);
        seats_ = options.seats;
    }
    if (live.seed) {
        scenario_.seed = *live.seed;
    }
    if (live.duration) {
        scenario_.duration = *live.duration;
    }
    outputs_.emplace(OutputOptions{.logPath = live.outputs.logPath,
                                   .logKinds = live.outputs.logKinds,
                                   .jsonPath = live.outputs.jsonPath},
                     scenario_);
    record_ = openForWriting(live.recordPath);
    report_ = openForWriting(live.reportPath);
    gateway_.emplace(scenario_, std::move(text), AgentRegistry::withBuiltIns(),
                     std::move(options), outputs_->sink());
}

void LiveSession::finish(std::ostream& out, std::string_view verb) {
    const Session& record = gateway_->session();
    outputs_->finish(record.end);
    out << std::format("{} (seed {}): {} {} of {} with {} actions\n", live_.path,
                       scenario_.seed, verb, formatDuration(record.end),
                       formatDuration(scenario_.duration), record.actions.size());
    Scenario played = scenario_;
    played.duration = record.end;
    const RunResult result = gateway_->result();
    printResults(out, played, result);
    outputs_->report(out, played, result);
    if (live_.recordPath) {
        writeSession(record_, record);
        if (!record_.flush()) {
            throw std::runtime_error(
                std::format("could not write all of '{}'", *live_.recordPath));
        }
        out << std::format("session written to {}; crowdbook replay plays it back\n",
                           *live_.recordPath);
    }
    if (live_.reportPath) {
        writeReport(report_, *live_.reportPath, record);
    }
}

} // namespace crowdbook

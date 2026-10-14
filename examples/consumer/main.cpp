// Runs a scenario, serves it to a client through the gateway in-process, and prints the client's
// result, then steps two markets of it through a learning pool: the core, the scenario files, the
// protocol, the gateway and the learning pool, all from the installed package.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "crowdbook/gateway.hpp"
#include "crowdbook/learning.hpp"
#include "crowdbook/protocol.hpp"
#include "crowdbook/scenario_file.hpp"
#include "crowdbook/version.hpp"

int main() {
    const std::string text = R"(duration = "5s"
[[agents]]
type = "zero_intelligence"
count = 10
)";
    const crowdbook::Scenario scenario = crowdbook::parseScenario(text);
    crowdbook::Gateway gateway{scenario, text, crowdbook::AgentRegistry::withBuiltIns(), {}};
    const crowdbook::ConnectionId client = gateway.connect(0);
    gateway.receive(client, crowdbook::protocol::encode(crowdbook::protocol::Hello{.seat = "you"}),
                    0);
    gateway.advance(10 * crowdbook::kSecond);
    std::cout << "crowdbook " << crowdbook::version() << ": the session "
              << (gateway.finished() ? "ended" : "did not end") << " after "
              << gateway.pendingOutput(client).size() << " bytes for the client\n";

    crowdbook::LearningPool pool{text, "consumer", 2, 2};
    pool.reset(std::vector<std::uint64_t>{1, 2});
    pool.step(std::vector<std::int64_t>{3, 0, 0, 0}); // both sides joined, and a hold
    // The market that held decides again a second after the first decision, at 2 s.
    const std::int64_t held = pool.state()[crowdbook::kLearningStateNames.size()];
    std::cout << "a learning pool stepped the market that held to " << held << " ns\n";
    return gateway.finished() && held == 2 * crowdbook::kSecond ? 0 : 1;
}

// Runs a scenario, serves it to a client through the gateway in-process, and prints the client's
// result: the core, the scenario files, the protocol and the gateway, all from the installed
// package.

#include <iostream>
#include <string>

#include "crowdbook/gateway.hpp"
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
    return gateway.finished() ? 0 : 1;
}

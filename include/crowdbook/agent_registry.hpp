#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "crowdbook/activity.hpp"
#include "crowdbook/agent.hpp"
#include "crowdbook/fundamental.hpp"
#include "crowdbook/parameters.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// What a scenario shares with every agent it creates, besides the agent's own parameters.
struct Environment {
    Price referencePrice = 10'000; // where agents anchor before they have seen any quotes
    std::shared_ptr<Fundamental> fundamental{}; // the asset's value, if the scenario models one
    ActivityCurve activity{}; // how busy the day is expected to be, flat without a trading day
};

using AgentFactory =
    std::function<std::unique_ptr<Agent>(const Parameters& parameters,
                                         const Environment& environment)>;

// Creates agents by type name, so scenario files can refer to them. Add your own agent types next
// to the built-in ones to use them in scenarios.
class AgentRegistry {
public:
    // A registry with the built-in types: zero_intelligence, market_maker, momentum and informed.
    [[nodiscard]] static AgentRegistry withBuiltIns();

    // Throws std::invalid_argument if the type name is already taken.
    void add(std::string type, AgentFactory factory);

    // Creates an agent. Throws std::invalid_argument for an unknown type or invalid parameters,
    // including any parameter the factory never read, which is almost always a typo.
    [[nodiscard]] std::unique_ptr<Agent> create(std::string_view type,
                                                const Parameters& parameters,
                                                const Environment& environment) const;

    // The registered type names, in alphabetical order.
    [[nodiscard]] std::vector<std::string> types() const;

private:
    std::map<std::string, AgentFactory, std::less<>> factories_;
};

} // namespace crowdbook

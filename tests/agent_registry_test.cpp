#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/agents/execution.hpp"
#include "crowdbook/agents/zero_intelligence.hpp"

namespace crowdbook {
namespace {

// The message of the std::invalid_argument that `create` throws, or "" if it does not throw.
std::string creationError(const AgentRegistry& registry, std::string_view type,
                          const Parameters& parameters, const Environment& environment = {}) {
    try {
        static_cast<void>(registry.create(type, parameters, environment));
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    return "";
}

class Idle final : public Agent {};

TEST(AgentRegistryTest, KnowsTheBuiltInTypes) {
    EXPECT_EQ(AgentRegistry::withBuiltIns().types(),
              (std::vector<std::string>{"adaptive", "execution", "informed", "market_maker",
                                        "momentum", "zero_intelligence"}));
}

TEST(AgentRegistryTest, CreatesBuiltInAgentsFromParameters) {
    const AgentRegistry registry = AgentRegistry::withBuiltIns();
    Parameters parameters;
    parameters.set("limit_rate", 4.0);
    parameters.set("max_size", std::int64_t{3});
    const std::unique_ptr<Agent> agent = registry.create("zero_intelligence", parameters, {});
    EXPECT_NE(dynamic_cast<ZeroIntelligenceTrader*>(agent.get()), nullptr);

    Parameters pov;
    pov.set("style", std::string{"pov"});
    pov.set("participation", 0.2);
    const std::unique_ptr<Agent> execution = registry.create("execution", pov, {});
    EXPECT_NE(dynamic_cast<ExecutionTrader*>(execution.get()), nullptr);

    for (const char* type : {"market_maker", "momentum", "execution"}) {
        EXPECT_NE(registry.create(type, Parameters{}, {}), nullptr) << type;
    }
    Environment withValue;
    withValue.fundamental = std::make_shared<Fundamental>(FundamentalConfig{}, Random{1, 0});
    EXPECT_NE(registry.create("informed", Parameters{}, withValue), nullptr);
}

TEST(AgentRegistryTest, RejectsUnknownTypesAndNamesTheKnownOnes) {
    const std::string error =
        creationError(AgentRegistry::withBuiltIns(), "market_taker", Parameters{});
    EXPECT_NE(error.find("unknown agent type 'market_taker'"), std::string::npos) << error;
    EXPECT_NE(error.find("market_maker"), std::string::npos) << error;
}

TEST(AgentRegistryTest, RejectsParametersTheAgentNeverReads) {
    Parameters parameters;
    parameters.set("limit_rate", 1.0);
    parameters.set("limt_rate", 2.0);
    const std::string error =
        creationError(AgentRegistry::withBuiltIns(), "zero_intelligence", parameters);
    EXPECT_NE(error.find("unknown parameter for agent type 'zero_intelligence': limt_rate"),
              std::string::npos)
        << error;
}

TEST(AgentRegistryTest, RejectsInvalidValues) {
    const AgentRegistry registry = AgentRegistry::withBuiltIns();
    Parameters badOffset;
    badOffset.set("max_offset", std::int64_t{0});
    EXPECT_NE(creationError(registry, "zero_intelligence", badOffset), "");

    Parameters wrongType;
    wrongType.set("limit_rate", std::string{"fast"});
    EXPECT_NE(creationError(registry, "zero_intelligence", wrongType), "");

    Parameters badStyle;
    badStyle.set("style", std::string{"vwap"});
    EXPECT_NE(creationError(registry, "execution", badStyle).find("\"twap\" or \"pov\""),
              std::string::npos);

    // Informed traders need the scenario to model a fundamental value.
    EXPECT_NE(creationError(registry, "informed", Parameters{}), "");
}

TEST(AgentRegistryTest, AcceptsUserDefinedTypes) {
    AgentRegistry registry = AgentRegistry::withBuiltIns();
    registry.add("idle", [](const Parameters&, const Environment&) {
        return std::make_unique<Idle>();
    });
    EXPECT_NE(registry.create("idle", Parameters{}, {}), nullptr);
    EXPECT_THROW(registry.add("idle", [](const Parameters&, const Environment&) {
        return std::make_unique<Idle>();
    }), std::invalid_argument);
    EXPECT_THROW(registry.add("empty", nullptr), std::invalid_argument);
}

} // namespace
} // namespace crowdbook

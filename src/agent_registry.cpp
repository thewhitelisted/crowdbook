#include "crowdbook/agent_registry.hpp"

#include <format>
#include <stdexcept>
#include <string>
#include <utility>

#include "crowdbook/agents/adaptive.hpp"
#include "crowdbook/agents/execution.hpp"
#include "crowdbook/agents/informed.hpp"
#include "crowdbook/agents/market_maker.hpp"
#include "crowdbook/agents/momentum.hpp"
#include "crowdbook/agents/zero_intelligence.hpp"

namespace crowdbook {

namespace {

std::string joined(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names) {
        text += text.empty() ? name : ", " + name;
    }
    return text;
}

std::unique_ptr<Agent> makeZeroIntelligence(const Parameters& parameters,
                                            const Environment& environment) {
    const ZeroIntelligenceConfig defaults;
    return std::make_unique<ZeroIntelligenceTrader>(
        ZeroIntelligenceConfig{
            .limitRate = parameters.number("limit_rate", defaults.limitRate),
            .marketRate = parameters.number("market_rate", defaults.marketRate),
            .cancelRate = parameters.number("cancel_rate", defaults.cancelRate),
            .maxOffset = parameters.integer("max_offset", defaults.maxOffset),
            .minSize = parameters.integer("min_size", defaults.minSize),
            .maxSize = parameters.integer("max_size", defaults.maxSize),
            .activityResponse = parameters.number("activity_response", defaults.activityResponse),
            .activityMemory = parameters.duration("activity_memory", defaults.activityMemory),
            .activityBaseline =
                parameters.duration("activity_baseline", defaults.activityBaseline),
            .volatilityResponse =
                parameters.number("volatility_response", defaults.volatilityResponse),
        },
        environment.referencePrice);
}

std::unique_ptr<Agent> makeMarketMaker(const Parameters& parameters,
                                       const Environment& environment) {
    const MarketMakerConfig defaults;
    return std::make_unique<MarketMaker>(
        MarketMakerConfig{
            .riskAversion = parameters.number("risk_aversion", defaults.riskAversion),
            .volatility = parameters.number("volatility", defaults.volatility),
            .intensity = parameters.number("intensity", defaults.intensity),
            .horizon = parameters.duration("horizon", defaults.horizon),
            .quoteSize = parameters.integer("quote_size", defaults.quoteSize),
            .maxInventory = parameters.integer("max_inventory", defaults.maxInventory),
            .requoteInterval = parameters.duration("requote_interval", defaults.requoteInterval),
            .fairValueWeight = parameters.number("fair_value_weight", defaults.fairValueWeight),
            .postOnly = parameters.flag("post_only", defaults.postOnly),
        },
        environment.referencePrice);
}

std::unique_ptr<Agent> makeAdaptive(const Parameters& parameters,
                                    const Environment& environment) {
    const AdaptiveConfig defaults;
    return std::make_unique<AdaptiveTrader>(
        AdaptiveConfig{
            .interval = parameters.duration("interval", defaults.interval),
            .noise = parameters.number("noise", defaults.noise),
            .fastHalfLife = parameters.duration("fast_half_life", defaults.fastHalfLife),
            .slowHalfLife = parameters.duration("slow_half_life", defaults.slowHalfLife),
            .memory = parameters.duration("memory", defaults.memory),
            .choiceIntensity = parameters.number("choice_intensity", defaults.choiceIntensity),
            .threshold = parameters.number("threshold", defaults.threshold),
            .orderSize = parameters.integer("order_size", defaults.orderSize),
            .maxPosition = parameters.integer("max_position", defaults.maxPosition),
        },
        environment.fundamental, environment.referencePrice);
}

std::unique_ptr<Agent> makeExecution(const Parameters& parameters,
                                     const Environment& /*environment*/) {
    const ExecutionConfig defaults;
    const std::string style = parameters.text("style", "twap");
    if (style != "twap" && style != "pov") {
        throw std::invalid_argument(
            std::format("execution style must be \"twap\" or \"pov\", not \"{}\"", style));
    }
    return std::make_unique<ExecutionTrader>(ExecutionConfig{
        .style = style == "twap" ? ExecutionStyle::Twap : ExecutionStyle::Pov,
        .minParent = parameters.integer("min_parent", defaults.minParent),
        .parentTail = parameters.number("parent_tail", defaults.parentTail),
        .maxParent = parameters.integer("max_parent", defaults.maxParent),
        .pause = parameters.duration("pause", defaults.pause),
        .interval = parameters.duration("interval", defaults.interval),
        .childSize = parameters.integer("child_size", defaults.childSize),
        .participation = parameters.number("participation", defaults.participation),
    });
}

std::unique_ptr<Agent> makeMomentum(const Parameters& parameters,
                                    const Environment& environment) {
    const MomentumConfig defaults;
    return std::make_unique<MomentumTrader>(
        MomentumConfig{
            .interval = parameters.duration("interval", defaults.interval),
            .fastHalfLife = parameters.duration("fast_half_life", defaults.fastHalfLife),
            .slowHalfLife = parameters.duration("slow_half_life", defaults.slowHalfLife),
            .threshold = parameters.number("threshold", defaults.threshold),
            .orderSize = parameters.integer("order_size", defaults.orderSize),
            .maxPosition = parameters.integer("max_position", defaults.maxPosition),
        },
        environment.referencePrice);
}

std::unique_ptr<Agent> makeInformed(const Parameters& parameters,
                                    const Environment& environment) {
    const InformedConfig defaults;
    return std::make_unique<InformedTrader>(
        InformedConfig{
            .interval = parameters.duration("interval", defaults.interval),
            .noise = parameters.number("noise", defaults.noise),
            .threshold = parameters.number("threshold", defaults.threshold),
            .orderSize = parameters.integer("order_size", defaults.orderSize),
            .maxPosition = parameters.integer("max_position", defaults.maxPosition),
        },
        environment.fundamental);
}

} // namespace

AgentRegistry AgentRegistry::withBuiltIns() {
    AgentRegistry registry;
    registry.add("zero_intelligence", makeZeroIntelligence);
    registry.add("adaptive", makeAdaptive);
    registry.add("execution", makeExecution);
    registry.add("market_maker", makeMarketMaker);
    registry.add("momentum", makeMomentum);
    registry.add("informed", makeInformed);
    return registry;
}

void AgentRegistry::add(std::string type, AgentFactory factory) {
    if (!factory) {
        throw std::invalid_argument(std::format("agent type '{}' needs a factory", type));
    }
    const auto [entry, inserted] = factories_.try_emplace(std::move(type), std::move(factory));
    if (!inserted) {
        throw std::invalid_argument(
            std::format("agent type '{}' is already registered", entry->first));
    }
}

std::unique_ptr<Agent> AgentRegistry::create(std::string_view type, const Parameters& parameters,
                                             const Environment& environment) const {
    const auto factory = factories_.find(type);
    if (factory == factories_.end()) {
        throw std::invalid_argument(std::format("unknown agent type '{}'; known types: {}", type,
                                                joined(types())));
    }
    std::unique_ptr<Agent> agent = factory->second(parameters, environment);
    if (!agent) {
        throw std::invalid_argument(std::format("the factory for '{}' made no agent", type));
    }
    if (const std::vector<std::string> unused = parameters.unusedNames(); !unused.empty()) {
        throw std::invalid_argument(std::format("unknown parameter{} for agent type '{}': {}",
                                                unused.size() == 1 ? "" : "s", type,
                                                joined(unused)));
    }
    return agent;
}

std::vector<std::string> AgentRegistry::types() const {
    std::vector<std::string> names;
    for (const auto& entry : factories_) {
        names.push_back(entry.first);
    }
    return names;
}

} // namespace crowdbook

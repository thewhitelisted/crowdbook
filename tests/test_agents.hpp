#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <utility>
#include <variant>
#include <vector>

#include "crowdbook/agent.hpp"
#include "crowdbook/event_log.hpp"
#include "crowdbook/simulation.hpp"
#include "exchange_test_support.hpp"

namespace crowdbook::test {

// An event as an agent received it, with the time it arrived.
struct Received {
    Timestamp time = 0;
    Event event{};

    friend bool operator==(const Received&, const Received&) = default;
};

inline void PrintTo(const Received& received, std::ostream* os) {
    *os << "at " << received.time << ": ";
    std::visit([os](const auto& event) { PrintTo(event, os); }, received.event);
}

// Records every callback with the time it ran, and runs optional hooks on start and wakeup.
class RecordingAgent final : public Agent {
public:
    std::function<void(AgentContext&)> startHook;
    std::function<void(AgentContext&, std::uint64_t)> wakeupHook;

    std::vector<Timestamp> starts;
    std::vector<std::pair<Timestamp, std::uint64_t>> wakeups;
    std::vector<Received> received;

    void onStart(AgentContext& context) override {
        starts.push_back(context.now());
        if (startHook) {
            startHook(context);
        }
    }

    void onWakeup(AgentContext& context, std::uint64_t tag) override {
        wakeups.emplace_back(context.now(), tag);
        if (wakeupHook) {
            wakeupHook(context, tag);
        }
    }

    void onAccepted(AgentContext& context, const OrderAccepted& event) override {
        record(context, event);
    }
    void onRejected(AgentContext& context, const OrderRejected& event) override {
        record(context, event);
    }
    void onModified(AgentContext& context, const OrderModified& event) override {
        record(context, event);
    }
    void onFilled(AgentContext& context, const OrderFilled& event) override {
        record(context, event);
    }
    void onCancelled(AgentContext& context, const OrderCancelled& event) override {
        record(context, event);
    }
    void onTrade(AgentContext& context, const Trade& trade) override { record(context, trade); }
    void onTopOfBook(AgentContext& context, const TopOfBook& top) override {
        record(context, top);
    }

private:
    void record(const AgentContext& context, Event event) {
        received.push_back({.time = context.now(), .event = std::move(event)});
    }
};

// Keeps everything the exchange saw, with the time it happened.
class RecordingSink final : public EventSink {
public:
    struct RequestRecord {
        Timestamp time = 0;
        AgentId agent = 0;
        Request request{};
    };

    std::vector<RequestRecord> requests;
    std::vector<std::pair<Timestamp, Event>> events;

    void onRequest(Timestamp time, AgentId agent, const Request& request) override {
        requests.push_back({.time = time, .agent = agent, .request = request});
    }
    void onEvent(Timestamp time, const Event& event) override { events.emplace_back(time, event); }
};

// Adds an agent and returns a reference to it, valid for as long as the simulation lives.
template <typename AgentType, typename... Args>
AgentType& add(Simulation& simulation, const AgentOptions& options, Args&&... args) {
    auto agent = std::make_unique<AgentType>(std::forward<Args>(args)...);
    AgentType& reference = *agent;
    simulation.addAgent(std::move(agent), options);
    return reference;
}

} // namespace crowdbook::test

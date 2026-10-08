// Holds the hot paths to what they allocate. Timings vary from machine to machine, but how often
// a run goes to the heap does not, so these tests can fail a build that slows the engine down:
// each runs a market until it is warm, then counts the allocations per request over a stretch
// of steady trading and compares them with a ceiling a little above what the code does now.
//
// This file replaces the global operator new, so it is a test program of its own.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <ostream>
#include <streambuf>
#include <string>

#include <gtest/gtest.h>

#include "crowdbook/agent_registry.hpp"
#include "crowdbook/event_log.hpp"
#include "crowdbook/gateway.hpp"
#include "crowdbook/protocol.hpp"
#include "crowdbook/scenario_file.hpp"
#include "crowdbook/session.hpp"

namespace {

std::atomic<std::int64_t> allocations{0};

} // namespace

void* operator new(std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t /*size*/) noexcept { std::free(memory); }

namespace crowdbook {
namespace {

std::string example(const std::string& path) { return std::string{CROWDBOOK_SOURCE_DIR} + path; }

// Counts the requests that reach the exchange.
class RequestCounter final : public EventSink {
public:
    void onRequest(Timestamp /*time*/, AgentId /*agent*/, const Request& /*request*/) override {
        ++requests;
    }
    void onEvent(Timestamp /*time*/, const Event& /*event*/) override {}

    std::int64_t requests = 0;
};

// Passes everything on to two sinks.
class Both final : public EventSink {
public:
    Both(EventSink& first, EventSink& second) : first_(first), second_(second) {}
    void onRequest(Timestamp time, AgentId agent, const Request& request) override {
        first_.onRequest(time, agent, request);
        second_.onRequest(time, agent, request);
    }
    void onEvent(Timestamp time, const Event& event) override {
        first_.onEvent(time, event);
        second_.onEvent(time, event);
    }

private:
    EventSink& first_;
    EventSink& second_;
};

// Keeps nothing written to it.
class Discard final : public std::streambuf {
protected:
    std::streamsize xsputn(const char* /*text*/, std::streamsize count) override { return count; }
    int_type overflow(int_type character) override { return character; }
};

constexpr Duration kWarm = 60 * kSecond;
constexpr Duration kMeasured = 300 * kSecond;

// Allocations per request while the market runs from kWarm to kWarm + kMeasured.
double allocationsPerRequest(const std::string& scenarioPath, EventSink* extra = nullptr) {
    Scenario scenario = loadScenario(example(scenarioPath));
    scenario.duration = kWarm + kMeasured;
    RequestCounter counter;
    Both both{counter, extra != nullptr ? *extra : counter};
    SessionMarket market = openSession(scenario, AgentRegistry::withBuiltIns(),
                                       extra != nullptr ? static_cast<EventSink*>(&both)
                                                        : &counter);
    market.run.runUntil(kWarm);
    const std::int64_t requestsBefore = counter.requests;
    const std::int64_t before = allocations.load();
    market.run.runUntil(kWarm + kMeasured);
    const std::int64_t allocated = allocations.load() - before;
    const std::int64_t requests = counter.requests - requestsBefore;
    EXPECT_GT(requests, 10'000) << "too quiet a market to measure";
    return static_cast<double>(allocated) / static_cast<double>(requests);
}

// A market with a depth feed: each depth update shares its levels with every copy of it, so a
// request costs at most one allocation for each side of the book it changes, and price levels
// coming and going cost the rest.
TEST(AllocationTest, AMarketWithADepthFeed) {
    const double perRequest = allocationsPerRequest("/examples/challenges/market_making.toml");
    RecordProperty("allocations_per_request", std::to_string(perRequest));
    EXPECT_LT(perRequest, 1.25) << "allocations per request: " << perRequest;
}

// A crowd of a thousand agents and no depth feed: orders, the book's nodes, the ledgers and the
// event queue reuse their memory, so only price levels coming and going allocate.
TEST(AllocationTest, ALargeMarket) {
    const double perRequest = allocationsPerRequest("/examples/scenarios/large_market.toml");
    RecordProperty("allocations_per_request", std::to_string(perRequest));
    EXPECT_LT(perRequest, 0.1) << "allocations per request: " << perRequest;
}

// The event log adds nothing: each row is built in a buffer that is reused.
TEST(AllocationTest, TheEventLogAllocatesNothingPerRow) {
    Discard discard;
    std::ostream out{&discard};
    CsvEventLog log{out};
    const double withLog = allocationsPerRequest("/examples/challenges/market_making.toml", &log);
    const double without = allocationsPerRequest("/examples/challenges/market_making.toml");
    EXPECT_LT(withLog - without, 0.01)
        << "allocations per request: " << withLog << " with the log, " << without << " without";
}

// Serving a seat adds next to nothing: each message is encoded straight into the connection's
// buffer, which keeps its memory as the client reads.
TEST(AllocationTest, ServingASeat) {
    Scenario scenario = loadScenario(example("/examples/challenges/market_making.toml"));
    scenario.duration = kWarm + kMeasured;
    RequestCounter counter;
    Gateway gateway{scenario, "", AgentRegistry::withBuiltIns(), {}, &counter};
    const ConnectionId client = gateway.connect(0);
    gateway.receive(client, protocol::encode(protocol::Hello{.seat = "you"}), 0);
    const auto runTo = [&](std::int64_t from, std::int64_t end) {
        for (std::int64_t wall = from; wall <= end; wall += 10 * kMillisecond) {
            gateway.advance(wall);
            gateway.consumeOutput(client, gateway.pendingOutput(client).size());
        }
    };
    runTo(0, kWarm);
    const std::int64_t requestsBefore = counter.requests;
    const std::int64_t before = allocations.load();
    runTo(kWarm + 10 * kMillisecond, kWarm + kMeasured);
    const double served = static_cast<double>(allocations.load() - before) /
                          static_cast<double>(counter.requests - requestsBefore);
    const double unserved = allocationsPerRequest("/examples/challenges/market_making.toml");
    RecordProperty("allocations_per_request", std::to_string(served));
    EXPECT_LT(served - unserved, 0.05)
        << "allocations per request: " << served << " served, " << unserved << " not";
}

} // namespace
} // namespace crowdbook

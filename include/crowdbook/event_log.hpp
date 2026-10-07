#pragma once

#include <functional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "crowdbook/messages.hpp"
#include "crowdbook/types.hpp"

namespace crowdbook {

// Receives everything that happens at the exchange, in order: each request as it arrives, followed
// by the events it produced.
class EventSink {
public:
    EventSink() = default;
    EventSink(const EventSink&) = delete;
    EventSink& operator=(const EventSink&) = delete;
    EventSink(EventSink&&) = delete;
    EventSink& operator=(EventSink&&) = delete;
    virtual ~EventSink() = default;

    virtual void onRequest(Timestamp time, AgentId agent, const Request& request) = 0;
    virtual void onEvent(Timestamp time, const Event& event) = 0;
};

// Writes the exchange's activity as CSV: a header, then one row per request or event. Columns that
// do not apply to a row are left empty. The `kind` column is one of new, cancel, modify, accepted,
// rejected, modified, filled, cancelled, trade and top_of_book.
class CsvEventLog final : public EventSink {
public:
    // Writes every row, or only rows of the given kinds when `kinds` is not empty. Throws
    // std::invalid_argument for a kind that does not exist.
    explicit CsvEventLog(std::ostream& out, const std::vector<std::string>& kinds = {});

    void onRequest(Timestamp time, AgentId agent, const Request& request) override;
    void onEvent(Timestamp time, const Event& event) override;

private:
    [[nodiscard]] bool keeps(std::string_view kind) const;

    std::ostream& out_;
    std::set<std::string, std::less<>> kinds_; // empty keeps every kind
};

// Writes the best bid, best ask and last trade price every `interval`, as CSV with the columns
// time, bid, ask and last_trade: a compact price history for long runs, where a full event log
// would be too large. Each row shows the market after everything up to and including its time.
class PriceSampler final : public EventSink {
public:
    // Throws std::invalid_argument unless the interval is positive.
    PriceSampler(std::ostream& out, Duration interval);

    void onRequest(Timestamp time, AgentId agent, const Request& request) override;
    void onEvent(Timestamp time, const Event& event) override;
    // Writes the rows still due up to and including `end`; call it when the run is over.
    void finish(Timestamp end);

private:
    void writeRowsBefore(Timestamp time);

    std::ostream& out_;
    Duration interval_;
    Timestamp nextRow_ = 0;
    MarketSnapshot market_;
};

// Passes every request and event on to several sinks, in the order they were added.
class BroadcastSink final : public EventSink {
public:
    void add(EventSink& sink) { sinks_.push_back(&sink); }

    void onRequest(Timestamp time, AgentId agent, const Request& request) override;
    void onEvent(Timestamp time, const Event& event) override;

private:
    std::vector<EventSink*> sinks_;
};

} // namespace crowdbook

#pragma once

#include <cstddef>
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

// Writes one row every `interval` from time 0 to the end of the run, each showing the market
// after everything up to and including its time: a compact history for long runs, where a full
// event log would be too large. Subclasses keep the state they report and format the rows.
class IntervalSampler : public EventSink {
public:
    void onRequest(Timestamp time, AgentId agent, const Request& request) final;
    void onEvent(Timestamp time, const Event& event) final;
    // Writes the rows still due up to and including `end`; call it when the run is over.
    void finish(Timestamp end);

protected:
    // Throws std::invalid_argument unless the interval is positive.
    explicit IntervalSampler(Duration interval);

private:
    virtual void update(const Event& event) = 0;
    virtual void writeRow(Timestamp time) = 0;
    void writeRowsBefore(Timestamp time);

    Duration interval_;
    Timestamp nextRow_ = 0;
};

// Writes the best bid, best ask and last trade price, as CSV with the columns time, bid, ask and
// last_trade. A price is empty while there is none.
class PriceSampler final : public IntervalSampler {
public:
    // Throws std::invalid_argument unless the interval is positive.
    PriceSampler(std::ostream& out, Duration interval);

private:
    void update(const Event& event) override;
    void writeRow(Timestamp time) override;

    std::ostream& out_;
    MarketSnapshot market_;
};

// Writes the exchange's depth feed, as CSV with the columns time, then bid_price_1,
// bid_quantity_1 up to level `levels`, then the same for asks. Levels the book does not have are
// empty. The exchange must publish a depth feed at least `levels` deep.
class DepthSampler final : public IntervalSampler {
public:
    // Throws std::invalid_argument unless the interval and the number of levels are positive.
    DepthSampler(std::ostream& out, Duration interval, std::size_t levels);

private:
    void update(const Event& event) override;
    void writeRow(Timestamp time) override;

    std::ostream& out_;
    std::size_t levels_;
    BookDepth depth_;
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

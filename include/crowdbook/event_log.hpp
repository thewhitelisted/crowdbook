#pragma once

#include <ostream>

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
    explicit CsvEventLog(std::ostream& out);

    void onRequest(Timestamp time, AgentId agent, const Request& request) override;
    void onEvent(Timestamp time, const Event& event) override;

private:
    std::ostream& out_;
};

} // namespace crowdbook

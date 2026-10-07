#include "crowdbook/event_log.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

#include "overloaded.hpp"

namespace crowdbook {

namespace {

// One CSV row; fields left empty are written as empty columns.
struct Row {
    Timestamp time = 0;
    std::string_view kind{};
    std::optional<std::uint64_t> agent{};
    std::optional<std::uint64_t> clientOrderId{};
    std::optional<std::uint64_t> orderId{};
    std::string_view side{};
    std::string_view type{};
    std::string_view timeInForce{};
    std::optional<Price> price{};
    std::optional<Quantity> quantity{};
    std::optional<Quantity> leaves{};
    std::string_view liquidity{};
    std::string_view request{};
    std::string_view reason{};
    std::optional<Price> bidPrice{};
    std::optional<Quantity> bidQuantity{};
    std::optional<Price> askPrice{};
    std::optional<Quantity> askQuantity{};
};

template <typename T>
void writeField(std::ostream& out, const std::optional<T>& value) {
    out << ',';
    if (value) {
        out << *value;
    }
}

void writeField(std::ostream& out, std::string_view value) { out << ',' << value; }

void write(std::ostream& out, const Row& row) {
    out << row.time << ',' << row.kind;
    writeField(out, row.agent);
    writeField(out, row.clientOrderId);
    writeField(out, row.orderId);
    writeField(out, row.side);
    writeField(out, row.type);
    writeField(out, row.timeInForce);
    writeField(out, row.price);
    writeField(out, row.quantity);
    writeField(out, row.leaves);
    writeField(out, row.liquidity);
    writeField(out, row.request);
    writeField(out, row.reason);
    writeField(out, row.bidPrice);
    writeField(out, row.bidQuantity);
    writeField(out, row.askPrice);
    writeField(out, row.askQuantity);
    out << '\n';
}

// Fills in the order type, and for limit orders the time in force and price.
void describeOrder(Row& row, OrderType type, TimeInForce timeInForce, Price price) {
    row.type = toString(type);
    if (type == OrderType::Limit) {
        row.timeInForce = toString(timeInForce);
        row.price = price;
    }
}

} // namespace

CsvEventLog::CsvEventLog(std::ostream& out) : out_(out) {
    out_ << "time,kind,agent,client_order_id,order_id,side,type,time_in_force,price,quantity,"
            "leaves,liquidity,request,reason,bid_price,bid_quantity,ask_price,ask_quantity\n";
}

void CsvEventLog::onRequest(Timestamp time, AgentId agent, const Request& request) {
    Row row{.time = time, .agent = agent, .clientOrderId = clientOrderIdOf(request)};
    if (const auto* order = std::get_if<NewOrder>(&request)) {
        row.kind = "new";
        row.side = toString(order->side);
        describeOrder(row, order->type, order->timeInForce, order->price);
        row.quantity = order->quantity;
    } else if (const auto* modify = std::get_if<ModifyOrder>(&request)) {
        row.kind = "modify";
        row.price = modify->price;
        row.quantity = modify->quantity;
    } else {
        row.kind = "cancel";
    }
    write(out_, row);
}

void CsvEventLog::onEvent(Timestamp time, const Event& event) {
    const Row row = std::visit(
        detail::Overloaded{
            [time](const OrderAccepted& accepted) {
                Row result{.time = time,
                           .kind = "accepted",
                           .agent = accepted.agent,
                           .clientOrderId = accepted.clientOrderId,
                           .orderId = accepted.orderId,
                           .side = toString(accepted.side),
                           .quantity = accepted.quantity};
                describeOrder(result, accepted.type, accepted.timeInForce, accepted.price);
                return result;
            },
            [time](const OrderRejected& rejected) {
                return Row{.time = time,
                           .kind = "rejected",
                           .agent = rejected.agent,
                           .clientOrderId = rejected.clientOrderId,
                           .request = toString(rejected.request),
                           .reason = toString(rejected.reason)};
            },
            [time](const OrderModified& modified) {
                return Row{.time = time,
                           .kind = "modified",
                           .agent = modified.agent,
                           .clientOrderId = modified.clientOrderId,
                           .orderId = modified.orderId,
                           .price = modified.price,
                           .quantity = modified.quantity};
            },
            [time](const OrderFilled& filled) {
                return Row{.time = time,
                           .kind = "filled",
                           .agent = filled.agent,
                           .clientOrderId = filled.clientOrderId,
                           .orderId = filled.orderId,
                           .side = toString(filled.side),
                           .price = filled.price,
                           .quantity = filled.quantity,
                           .leaves = filled.leavesQuantity,
                           .liquidity = toString(filled.liquidity)};
            },
            [time](const OrderCancelled& cancelled) {
                return Row{.time = time,
                           .kind = "cancelled",
                           .agent = cancelled.agent,
                           .clientOrderId = cancelled.clientOrderId,
                           .orderId = cancelled.orderId,
                           .quantity = cancelled.quantity,
                           .reason = toString(cancelled.reason)};
            },
            [time](const Trade& trade) {
                return Row{.time = time,
                           .kind = "trade",
                           .side = toString(trade.aggressorSide),
                           .price = trade.price,
                           .quantity = trade.quantity};
            },
            [time](const TopOfBook& top) {
                Row result{.time = time, .kind = "top_of_book"};
                if (top.bid) {
                    result.bidPrice = top.bid->price;
                    result.bidQuantity = top.bid->quantity;
                }
                if (top.ask) {
                    result.askPrice = top.ask->price;
                    result.askQuantity = top.ask->quantity;
                }
                return result;
            },
        },
        event);
    write(out_, row);
}

} // namespace crowdbook

#include "crowdbook/exchange.hpp"

#include <cstddef>
#include <format>
#include <stdexcept>
#include <utility>

namespace crowdbook {

namespace {

bool validLimitPrice(Price price) { return price >= 1 && price <= kMaxPrice; }

Quantity& openQuantity(Account& account, Side side) {
    return side == Side::Buy ? account.openBuyQuantity : account.openSellQuantity;
}

// Whether `additional` open quantity on `side` could take the position past `maxPosition` if
// every open order on that side filled.
bool breachesPositionLimit(const Account& account, Quantity maxPosition, Side side,
                           Quantity additional) {
    if (side == Side::Buy) {
        return account.position + account.openBuyQuantity + additional > maxPosition;
    }
    return account.position - account.openSellQuantity - additional < -maxPosition;
}

void settleSide(Account& account, Side side, Quantity quantity, Cash notional) {
    if (side == Side::Buy) {
        account.position += quantity;
        account.cash -= notional;
    } else {
        account.position -= quantity;
        account.cash += notional;
    }
}

} // namespace

void Exchange::addAgent(AgentId agent, const AccountConfig& config) {
    if (config.maxPosition < 0 || config.maxPosition > kMaxQuantity) {
        throw std::invalid_argument("maxPosition must be between 0 and kMaxQuantity");
    }
    if (config.maxOrderQuantity < 1 || config.maxOrderQuantity > kMaxQuantity) {
        throw std::invalid_argument("maxOrderQuantity must be between 1 and kMaxQuantity");
    }
    if (config.initialPosition < -config.maxPosition ||
        config.initialPosition > config.maxPosition) {
        throw std::invalid_argument("initialPosition is outside the position limit");
    }
    const AgentState state{.config = config,
                           .account = {.cash = config.initialCash,
                                       .position = config.initialPosition}};
    if (!agents_.try_emplace(agent, state).second) {
        throw std::invalid_argument(std::format("agent {} already has an account", agent));
    }
}

void Exchange::handle(AgentId agent, const Request& request, std::vector<Event>& events) {
    const auto found = agents_.find(agent);
    if (found == agents_.end()) {
        events.push_back(OrderRejected{.agent = agent,
                                       .clientOrderId = clientOrderIdOf(request),
                                       .request = kindOf(request),
                                       .reason = RejectReason::UnknownAgent});
        return;
    }

    AgentState& state = found->second;
    if (const auto* newOrder = std::get_if<NewOrder>(&request)) {
        submit(agent, state, *newOrder, events);
    } else if (const auto* cancelOrder = std::get_if<CancelOrder>(&request)) {
        cancel(agent, state, *cancelOrder, events);
    } else {
        modify(agent, state, std::get<ModifyOrder>(request), events);
    }
    publishTopOfBook(events);
}

const Account& Exchange::account(AgentId agent) const { return agents_.at(agent).account; }

std::optional<OrderId> Exchange::liveOrderId(AgentId agent, ClientOrderId clientOrderId) const {
    const auto state = agents_.find(agent);
    if (state == agents_.end()) {
        return std::nullopt;
    }
    const auto live = state->second.liveOrders.find(clientOrderId);
    if (live == state->second.liveOrders.end()) {
        return std::nullopt;
    }
    return live->second;
}

TopOfBook Exchange::topOfBook() const {
    return {.bid = book_.bestLevel(Side::Buy), .ask = book_.bestLevel(Side::Sell)};
}

std::optional<std::string> Exchange::audit() const {
    if (auto problem = book_.audit()) {
        return "order book: " + *problem;
    }

    std::size_t agentLiveOrders = 0;
    Cash cash = 0;
    Cash initialCash = 0;
    Quantity position = 0;
    Quantity initialPosition = 0;
    for (const auto& [agent, state] : agents_) {
        Quantity openBuy = 0;
        Quantity openSell = 0;
        for (const auto& [clientOrderId, id] : state.liveOrders) {
            const auto live = liveOrders_.find(id);
            if (live == liveOrders_.end() || live->second.agent != agent ||
                live->second.clientOrderId != clientOrderId) {
                return std::format("agent {} order {} is not indexed", agent, clientOrderId);
            }
            const std::optional<RestingOrder> resting = book_.find(id);
            if (!resting || resting->owner != agent || resting->side != live->second.side) {
                return std::format("agent {} order {} does not match the book", agent,
                                   clientOrderId);
            }
            if (resting->side == Side::Buy) {
                openBuy += resting->remaining;
            } else {
                openSell += resting->remaining;
            }
        }

        const Account& balances = state.account;
        if (balances.openBuyQuantity != openBuy || balances.openSellQuantity != openSell) {
            return std::format("agent {} records {}/{} open but its orders hold {}/{}", agent,
                               balances.openBuyQuantity, balances.openSellQuantity, openBuy,
                               openSell);
        }
        const Quantity limit = state.config.maxPosition;
        if (balances.position + openBuy > limit || balances.position - openSell < -limit) {
            return std::format("agent {} could breach its position limit of {}", agent, limit);
        }

        agentLiveOrders += state.liveOrders.size();
        cash += balances.cash;
        initialCash += state.config.initialCash;
        position += balances.position;
        initialPosition += state.config.initialPosition;
    }

    if (agentLiveOrders != liveOrders_.size() || liveOrders_.size() != book_.orderCount()) {
        return std::format("{} live orders by agent, {} indexed, {} in the book",
                           agentLiveOrders, liveOrders_.size(), book_.orderCount());
    }
    if (cash != initialCash) {
        return std::format("cash is not conserved: {} now, {} at the start", cash, initialCash);
    }
    if (position != initialPosition) {
        return std::format("shares are not conserved: {} now, {} at the start", position,
                           initialPosition);
    }
    return std::nullopt;
}

void Exchange::submit(AgentId agent, AgentState& state, const NewOrder& order,
                      std::vector<Event>& events) {
    const auto reject = [&](RejectReason reason) {
        events.push_back(OrderRejected{.agent = agent,
                                       .clientOrderId = order.clientOrderId,
                                       .request = RequestKind::New,
                                       .reason = reason});
    };
    if (order.quantity <= 0) {
        return reject(RejectReason::NonPositiveQuantity);
    }
    if (order.quantity > state.config.maxOrderQuantity) {
        return reject(RejectReason::OrderSizeLimit);
    }
    if (order.type == OrderType::Limit && !validLimitPrice(order.price)) {
        return reject(RejectReason::InvalidPrice);
    }
    if (state.liveOrders.contains(order.clientOrderId)) {
        return reject(RejectReason::DuplicateClientOrderId);
    }
    if (breachesPositionLimit(state.account, state.config.maxPosition, order.side,
                              order.quantity)) {
        return reject(RejectReason::PositionLimit);
    }

    const IncomingOrder incoming{.agent = agent,
                                 .clientOrderId = order.clientOrderId,
                                 .id = nextOrderId_++,
                                 .side = order.side,
                                 .quantity = order.quantity};
    events.push_back(OrderAccepted{.agent = agent,
                                   .clientOrderId = order.clientOrderId,
                                   .orderId = incoming.id,
                                   .side = order.side,
                                   .type = order.type,
                                   .timeInForce = order.timeInForce,
                                   .price = order.price,
                                   .quantity = order.quantity});

    fills_.clear();
    const OrderResult result = book_.submit({.id = incoming.id,
                                             .owner = agent,
                                             .side = order.side,
                                             .type = order.type,
                                             .timeInForce = order.timeInForce,
                                             .price = order.price,
                                             .quantity = order.quantity},
                                            fills_);
    settle(incoming, events);
    finish(incoming, state, result, events);
}

void Exchange::cancel(AgentId agent, AgentState& state, const CancelOrder& request,
                      std::vector<Event>& events) {
    const auto live = state.liveOrders.find(request.clientOrderId);
    if (live == state.liveOrders.end()) {
        events.push_back(OrderRejected{.agent = agent,
                                       .clientOrderId = request.clientOrderId,
                                       .request = RequestKind::Cancel,
                                       .reason = RejectReason::UnknownOrderId});
        return;
    }

    const OrderId id = live->second;
    const std::optional<RestingOrder> cancelled = book_.cancel(id);
    if (!cancelled) {
        throw std::logic_error(std::format("live order {} is missing from the book", id));
    }
    openQuantity(state.account, cancelled->side) -= cancelled->remaining;
    state.liveOrders.erase(live);
    liveOrders_.erase(id);
    events.push_back(OrderCancelled{.agent = agent,
                                    .clientOrderId = request.clientOrderId,
                                    .orderId = id,
                                    .quantity = cancelled->remaining,
                                    .reason = CancelReason::Requested});
}

void Exchange::modify(AgentId agent, AgentState& state, const ModifyOrder& request,
                      std::vector<Event>& events) {
    const auto reject = [&](RejectReason reason) {
        events.push_back(OrderRejected{.agent = agent,
                                       .clientOrderId = request.clientOrderId,
                                       .request = RequestKind::Modify,
                                       .reason = reason});
    };
    const auto live = state.liveOrders.find(request.clientOrderId);
    if (live == state.liveOrders.end()) {
        return reject(RejectReason::UnknownOrderId);
    }
    if (request.quantity <= 0) {
        return reject(RejectReason::NonPositiveQuantity);
    }
    if (request.quantity > state.config.maxOrderQuantity) {
        return reject(RejectReason::OrderSizeLimit);
    }
    if (!validLimitPrice(request.price)) {
        return reject(RejectReason::InvalidPrice);
    }

    const OrderId id = live->second;
    const std::optional<RestingOrder> current = book_.find(id);
    if (!current) {
        throw std::logic_error(std::format("live order {} is missing from the book", id));
    }
    if (request.quantity > current->remaining &&
        breachesPositionLimit(state.account, state.config.maxPosition, current->side,
                              request.quantity - current->remaining)) {
        return reject(RejectReason::PositionLimit);
    }

    const IncomingOrder incoming{.agent = agent,
                                 .clientOrderId = request.clientOrderId,
                                 .id = id,
                                 .side = current->side,
                                 .quantity = request.quantity,
                                 .restingBefore = current->remaining};
    events.push_back(OrderModified{.agent = agent,
                                   .clientOrderId = request.clientOrderId,
                                   .orderId = id,
                                   .price = request.price,
                                   .quantity = request.quantity});

    fills_.clear();
    const OrderResult result = book_.modify(id, request.price, request.quantity, fills_);
    settle(incoming, events);
    finish(incoming, state, result, events);
}

void Exchange::settle(const IncomingOrder& incoming, std::vector<Event>& events) {
    Quantity takerLeaves = incoming.quantity;
    for (const Fill& fill : fills_) {
        const LiveOrder maker = liveOrders_.at(fill.makerOrderId);
        AgentState& makerState = agents_.at(fill.makerOwner);
        AgentState& takerState = agents_.at(fill.takerOwner);
        const Side makerSide = opposite(fill.takerSide);
        const Cash notional = fill.price * fill.quantity;

        settleSide(makerState.account, makerSide, fill.quantity, notional);
        settleSide(takerState.account, fill.takerSide, fill.quantity, notional);
        openQuantity(makerState.account, makerSide) -= fill.quantity;
        takerLeaves -= fill.quantity;
        if (fill.makerRemaining == 0) {
            makerState.liveOrders.erase(maker.clientOrderId);
            liveOrders_.erase(fill.makerOrderId);
        }

        events.push_back(OrderFilled{.agent = fill.makerOwner,
                                     .clientOrderId = maker.clientOrderId,
                                     .orderId = fill.makerOrderId,
                                     .side = makerSide,
                                     .price = fill.price,
                                     .quantity = fill.quantity,
                                     .leavesQuantity = fill.makerRemaining,
                                     .liquidity = Liquidity::Maker});
        events.push_back(OrderFilled{.agent = fill.takerOwner,
                                     .clientOrderId = incoming.clientOrderId,
                                     .orderId = fill.takerOrderId,
                                     .side = fill.takerSide,
                                     .price = fill.price,
                                     .quantity = fill.quantity,
                                     .leavesQuantity = takerLeaves,
                                     .liquidity = Liquidity::Taker});
        events.push_back(
            Trade{.price = fill.price, .quantity = fill.quantity, .aggressorSide = fill.takerSide});
    }
}

void Exchange::finish(const IncomingOrder& incoming, AgentState& state, const OrderResult& result,
                      std::vector<Event>& events) {
    if (result.status == OrderStatus::Rejected) {
        throw std::logic_error(
            std::format("order book rejected validated order {}: {}", incoming.id,
                        toString(result.rejectReason)));
    }

    const Quantity restingAfter = result.status == OrderStatus::Resting ? result.remaining : 0;
    openQuantity(state.account, incoming.side) += restingAfter - incoming.restingBefore;
    if (restingAfter > 0) {
        state.liveOrders.insert_or_assign(incoming.clientOrderId, incoming.id);
        liveOrders_.insert_or_assign(incoming.id, LiveOrder{.agent = incoming.agent,
                                                            .clientOrderId = incoming.clientOrderId,
                                                            .side = incoming.side});
    } else {
        state.liveOrders.erase(incoming.clientOrderId);
        liveOrders_.erase(incoming.id);
    }

    if (result.status == OrderStatus::Cancelled) {
        events.push_back(OrderCancelled{.agent = incoming.agent,
                                        .clientOrderId = incoming.clientOrderId,
                                        .orderId = incoming.id,
                                        .quantity = incoming.quantity - result.filled,
                                        .reason = result.cancelReason});
    }
}

void Exchange::publishTopOfBook(std::vector<Event>& events) {
    TopOfBook top = topOfBook();
    if (top != publishedTop_) {
        publishedTop_ = top;
        events.push_back(std::move(top));
    }
}

} // namespace crowdbook

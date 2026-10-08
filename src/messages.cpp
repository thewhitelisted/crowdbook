#include "crowdbook/messages.hpp"

namespace crowdbook {

RequestKind kindOf(const Request& request) {
    if (std::holds_alternative<NewOrder>(request)) {
        return RequestKind::New;
    }
    if (std::holds_alternative<CancelOrder>(request)) {
        return RequestKind::Cancel;
    }
    return RequestKind::Modify;
}

ClientOrderId clientOrderIdOf(const Request& request) {
    return std::visit([](const auto& body) { return body.clientOrderId; }, request);
}

std::optional<AgentId> recipient(const Event& event) {
    return std::visit(
        [](const auto& body) -> std::optional<AgentId> {
            if constexpr (requires { body.agent; }) {
                return body.agent;
            } else {
                return std::nullopt;
            }
        },
        event);
}

std::string_view toString(RequestKind kind) noexcept {
    switch (kind) {
    case RequestKind::New:
        return "new";
    case RequestKind::Cancel:
        return "cancel";
    case RequestKind::Modify:
        return "modify";
    }
    return "unknown";
}

std::string_view toString(Liquidity liquidity) noexcept {
    switch (liquidity) {
    case Liquidity::Maker:
        return "maker";
    case Liquidity::Taker:
        return "taker";
    case Liquidity::Auction:
        return "auction";
    }
    return "unknown";
}

} // namespace crowdbook

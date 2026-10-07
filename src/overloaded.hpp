#pragma once

namespace crowdbook::detail {

// Builds a visitor for std::visit from one lambda per alternative.
template <typename... Handlers>
struct Overloaded : Handlers... {
    using Handlers::operator()...;
};

} // namespace crowdbook::detail

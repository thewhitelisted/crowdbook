#pragma once

#include <string_view>

namespace crowdbook {

// Library version as MAJOR.MINOR.PATCH, taken from the CMake project version.
[[nodiscard]] std::string_view version() noexcept;

} // namespace crowdbook

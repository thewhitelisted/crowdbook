#include "crowdbook/version.hpp"

#ifndef CROWDBOOK_VERSION
#error "CROWDBOOK_VERSION must be defined by the build system"
#endif

namespace crowdbook {

std::string_view version() noexcept { return CROWDBOOK_VERSION; }

} // namespace crowdbook

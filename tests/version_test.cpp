#include <regex>
#include <string>

#include <gtest/gtest.h>

#include "crowdbook/version.hpp"

namespace {

TEST(Version, IsMajorMinorPatch) {
    const std::string version{crowdbook::version()};
    EXPECT_TRUE(std::regex_match(version, std::regex{R"(\d+\.\d+\.\d+)"})) << version;
}

} // namespace

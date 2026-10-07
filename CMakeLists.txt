cmake_minimum_required(VERSION 3.25)

project(crowdbook
    VERSION 0.7.0
    DESCRIPTION "Agent-based limit order book simulator for market microstructure research"
    LANGUAGES CXX)

option(CROWDBOOK_BUILD_TESTS "Build the crowdbook test suite" ${PROJECT_IS_TOP_LEVEL})
option(CROWDBOOK_BUILD_APPS "Build the crowdbook command-line tool" ${PROJECT_IS_TOP_LEVEL})
option(CROWDBOOK_BUILD_EXAMPLES "Build the example programs" ${PROJECT_IS_TOP_LEVEL})
option(CROWDBOOK_BUILD_BENCHMARKS "Build the crowdbook benchmarks" OFF)
option(CROWDBOOK_BUILD_SCENARIOS "Build TOML scenario file support (fetches toml++)"
    ${PROJECT_IS_TOP_LEVEL})
option(CROWDBOOK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(CROWDBOOK_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if((CROWDBOOK_BUILD_TESTS OR CROWDBOOK_BUILD_APPS OR CROWDBOOK_BUILD_EXAMPLES)
   AND NOT CROWDBOOK_BUILD_SCENARIOS)
    message(FATAL_ERROR
        "The tests, the crowdbook tool and the examples need CROWDBOOK_BUILD_SCENARIOS=ON")
endif()

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CrowdbookWarnings)
include(CrowdbookSanitizers)

# A seeded run must come out the same on every platform, so floating-point expressions are
# evaluated exactly as written, everywhere: Clang fuses a * b + c into one rounding by default, and
# GCC on x86-64 does not. Set before any target, third-party ones included.
if(MSVC)
    add_compile_options(/fp:precise)
else()
    add_compile_options(-ffp-contract=off)
endif()

if(CROWDBOOK_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src)

if(CROWDBOOK_BUILD_APPS)
    add_subdirectory(apps)
endif()

if(CROWDBOOK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(CROWDBOOK_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(CROWDBOOK_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

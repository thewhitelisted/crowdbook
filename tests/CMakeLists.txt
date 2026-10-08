include(FetchContent)

FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    SYSTEM)
set(INSTALL_GTEST OFF)
FetchContent_MakeAvailable(googletest)

add_executable(crowdbook_tests
    adaptive_test.cpp
    agent_registry_test.cpp
    auction_test.cpp
    event_log_test.cpp
    execution_test.cpp
    exchange_random_test.cpp
    exchange_test.cpp
    fundamental_test.cpp
    gateway_test.cpp
    golden_test.cpp
    informed_test.cpp
    json_test.cpp
    ledger_test.cpp
    math_test.cpp
    live_test.cpp
    market_maker_test.cpp
    momentum_test.cpp
    order_book_differential_test.cpp
    order_book_test.cpp
    parameters_test.cpp
    protocol_test.cpp
    random_test.cpp
    report_test.cpp
    scenario_file_test.cpp
    scenario_test.cpp
    scoring_test.cpp
    server_test.cpp
    session_test.cpp
    trading_day_test.cpp
    simulation_determinism_test.cpp
    simulation_test.cpp
    version_test.cpp
    zero_intelligence_test.cpp)
target_link_libraries(crowdbook_tests PRIVATE
    crowdbook::gateway crowdbook::scenario crowdbook::protocol GTest::gtest_main)
# The JSON reader is internal to the protocol library; its tests reach it directly.
target_include_directories(crowdbook_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
# The golden test replays a session from examples/.
target_compile_definitions(crowdbook_tests PRIVATE CROWDBOOK_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(TARGET crowdbook_ladder)
    target_sources(crowdbook_tests PRIVATE ladder_test.cpp remote_test.cpp terminal_codes_test.cpp
        trading_screen_test.cpp)
    target_link_libraries(crowdbook_tests PRIVATE crowdbook_ladder)
endif()
crowdbook_set_warnings(crowdbook_tests)

include(GoogleTest)
gtest_discover_tests(crowdbook_tests)

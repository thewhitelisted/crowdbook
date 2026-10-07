add_library(crowdbook
    agent_registry.cpp
    agents/informed.cpp
    agents/market_maker.cpp
    agents/market_view.cpp
    agents/momentum.cpp
    agents/zero_intelligence.cpp
    event_log.cpp
    exchange.cpp
    fundamental.cpp
    ledger.cpp
    messages.cpp
    order_book.cpp
    parameters.cpp
    random.cpp
    scenario.cpp
    simulation.cpp
    types.cpp
    version.cpp)
add_library(crowdbook::crowdbook ALIAS crowdbook)

target_include_directories(crowdbook PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_compile_features(crowdbook PUBLIC cxx_std_23)
target_compile_definitions(crowdbook PRIVATE CROWDBOOK_VERSION="${PROJECT_VERSION}")
crowdbook_set_warnings(crowdbook)

# Scenario files are the only part that needs a third-party library, so they get their own target
# and the core library stays dependency-free.
if(CROWDBOOK_BUILD_SCENARIOS)
    include(FetchContent)
    FetchContent_Declare(tomlplusplus
        URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
        URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
        SYSTEM)
    FetchContent_MakeAvailable(tomlplusplus)

    add_library(crowdbook_scenario
        scenario_file.cpp)
    add_library(crowdbook::scenario ALIAS crowdbook_scenario)
    target_link_libraries(crowdbook_scenario
        PUBLIC crowdbook::crowdbook
        PRIVATE tomlplusplus::tomlplusplus)
    crowdbook_set_warnings(crowdbook_scenario)
endif()

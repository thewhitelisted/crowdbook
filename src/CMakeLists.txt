add_library(crowdbook
    agent_registry.cpp
    agents/adaptive.cpp
    agents/execution.cpp
    agents/informed.cpp
    agents/market_maker.cpp
    agents/market_view.cpp
    agents/momentum.cpp
    agents/participant.cpp
    agents/zero_intelligence.cpp
    event_log.cpp
    exchange.cpp
    fundamental.cpp
    ledger.cpp
    live.cpp
    math.cpp
    messages.cpp
    order_book.cpp
    parameters.cpp
    random.cpp
    scenario.cpp
    scoring.cpp
    simulation.cpp
    types.cpp
    version.cpp)
add_library(crowdbook::crowdbook ALIAS crowdbook)

target_include_directories(crowdbook PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(crowdbook PUBLIC cxx_std_23)
target_compile_definitions(crowdbook PRIVATE CROWDBOOK_VERSION="${PROJECT_VERSION}")
crowdbook_set_warnings(crowdbook)

# The protocol for clients of a served market: its messages and their encoding. It needs nothing
# but the core library, so clients can link it without the scenario files' dependency.
add_library(crowdbook_protocol
    json.cpp
    protocol.cpp)
add_library(crowdbook::protocol ALIAS crowdbook_protocol)
target_link_libraries(crowdbook_protocol PUBLIC crowdbook::crowdbook)
crowdbook_set_warnings(crowdbook_protocol)

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
        report.cpp
        scenario_file.cpp
        session.cpp)
    add_library(crowdbook::scenario ALIAS crowdbook_scenario)
    # toml++ is header-only and compiled into this library, so the installed package does not
    # depend on it.
    # Reports run their replays on several threads.
    find_package(Threads REQUIRED)
    target_link_libraries(crowdbook_scenario
        PUBLIC crowdbook::crowdbook Threads::Threads
        PRIVATE crowdbook::protocol $<BUILD_INTERFACE:tomlplusplus::tomlplusplus>)
    crowdbook_set_warnings(crowdbook_scenario)

    # Serving a market to clients over the protocol: the gateway, which works on bytes, and the
    # server, which moves them between the gateway and POSIX sockets.
    add_library(crowdbook_gateway
        gateway.cpp
        server.cpp)
    add_library(crowdbook::gateway ALIAS crowdbook_gateway)
    target_link_libraries(crowdbook_gateway PUBLIC crowdbook::scenario crowdbook::protocol)
    crowdbook_set_warnings(crowdbook_gateway)
endif()

# The installed package: the libraries, their headers, and a CMake config so that another project
# can find_package(crowdbook) and link crowdbook::crowdbook, crowdbook::protocol and, with scenario
# files, crowdbook::scenario and crowdbook::gateway.
include(CMakePackageConfigHelpers)
set(installed crowdbook crowdbook_protocol)
if(CROWDBOOK_BUILD_SCENARIOS)
    list(APPEND installed crowdbook_scenario crowdbook_gateway)
endif()
foreach(target IN LISTS installed)
    string(REPLACE "crowdbook_" "" name "${target}")
    set_target_properties(${target} PROPERTIES EXPORT_NAME ${name})
endforeach()
install(TARGETS ${installed} EXPORT crowdbookTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/crowdbook"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT crowdbookTargets
    NAMESPACE crowdbook::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/crowdbook)
configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/crowdbookConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/crowdbookConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/crowdbook)
# Until 1.0, a minor version may change the API, so only the same minor version is compatible.
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/crowdbookConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/crowdbookConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/crowdbookConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/crowdbook)

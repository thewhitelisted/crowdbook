add_executable(crowdbook_cli
    crowdbook/main.cpp)
set_target_properties(crowdbook_cli PROPERTIES OUTPUT_NAME crowdbook)
target_link_libraries(crowdbook_cli PRIVATE crowdbook::scenario)
crowdbook_set_warnings(crowdbook_cli)

if(CROWDBOOK_BUILD_TESTS)
    add_test(NAME cli.version COMMAND crowdbook_cli --version)
    set_tests_properties(cli.version PROPERTIES
        PASS_REGULAR_EXPRESSION "^crowdbook [0-9]+\\.[0-9]+\\.[0-9]+")
    add_test(NAME cli.agents COMMAND crowdbook_cli agents)
    set_tests_properties(cli.agents PROPERTIES PASS_REGULAR_EXPRESSION "market_maker")
    add_test(NAME cli.no_command COMMAND crowdbook_cli)
    add_test(NAME cli.missing_scenario COMMAND crowdbook_cli run no-such-scenario.toml)
    add_test(NAME cli.unknown_log_kind
        COMMAND crowdbook_cli run "${PROJECT_SOURCE_DIR}/examples/scenarios/market_maker.toml"
            --duration 1s --log "${CMAKE_CURRENT_BINARY_DIR}/unused.csv" --log-only trades)
    set_tests_properties(cli.no_command cli.missing_scenario cli.unknown_log_kind
        PROPERTIES WILL_FAIL TRUE)
    add_test(NAME cli.json_and_filtered_log
        COMMAND crowdbook_cli run "${PROJECT_SOURCE_DIR}/examples/scenarios/market_maker.toml"
            --duration 1s --json "${CMAKE_CURRENT_BINARY_DIR}/result.json"
            --log "${CMAKE_CURRENT_BINARY_DIR}/trades.csv" --log-only trade,top_of_book)
    set_tests_properties(cli.json_and_filtered_log PROPERTIES
        PASS_REGULAR_EXPRESSION "results written to")

    # Every example scenario has to keep working: run each one briefly, with an event log.
    file(GLOB examples CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/examples/scenarios/*.toml")
    foreach(example IN LISTS examples)
        get_filename_component(name "${example}" NAME_WE)
        add_test(NAME cli.example.${name}
            COMMAND crowdbook_cli run "${example}" --duration 2s
                --log "${CMAKE_CURRENT_BINARY_DIR}/${name}.csv")
    endforeach()
endif()

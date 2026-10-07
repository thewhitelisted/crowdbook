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
    set_tests_properties(cli.no_command cli.missing_scenario PROPERTIES WILL_FAIL TRUE)

    # Every example scenario has to keep working: run each one briefly, with an event log.
    file(GLOB examples CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/examples/scenarios/*.toml")
    foreach(example IN LISTS examples)
        get_filename_component(name "${example}" NAME_WE)
        add_test(NAME cli.example.${name}
            COMMAND crowdbook_cli run "${example}" --duration 2s
                --log "${CMAKE_CURRENT_BINARY_DIR}/${name}.csv")
    endforeach()
endif()

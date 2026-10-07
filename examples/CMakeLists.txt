add_executable(crowdbook_example_custom_agent
    custom_agent.cpp)
set_target_properties(crowdbook_example_custom_agent PROPERTIES OUTPUT_NAME custom_agent)
target_link_libraries(crowdbook_example_custom_agent PRIVATE crowdbook::scenario)
crowdbook_set_warnings(crowdbook_example_custom_agent)

if(CROWDBOOK_BUILD_TESTS)
    add_test(NAME example.custom_agent COMMAND crowdbook_example_custom_agent)
    set_tests_properties(example.custom_agent PROPERTIES
        PASS_REGULAR_EXPRESSION "reverter: position")
endif()

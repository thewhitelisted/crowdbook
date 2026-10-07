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

# The installed package works for a project of its own: install this build into the build tree,
# then configure, build and run examples/consumer against it. Skipped under the sanitizers, whose
# instrumented libraries a plain consumer cannot link.
if(CROWDBOOK_BUILD_TESTS AND NOT CROWDBOOK_SANITIZE)
    set(prefix "${PROJECT_BINARY_DIR}/package")
    set(consumer "${PROJECT_BINARY_DIR}/consumer")
    add_test(NAME package.install
        COMMAND "${CMAKE_COMMAND}" --install "${PROJECT_BINARY_DIR}" --prefix "${prefix}")
    add_test(NAME package.consumer_configure
        COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/consumer" -B "${consumer}"
            -G Ninja -DCMAKE_PREFIX_PATH=${prefix} -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER} -DCMAKE_CXX_FLAGS=-ffp-contract=off)
    add_test(NAME package.consumer_build COMMAND "${CMAKE_COMMAND}" --build "${consumer}")
    add_test(NAME package.consumer_run COMMAND "${consumer}/consumer")
    set_tests_properties(package.install PROPERTIES FIXTURES_SETUP package)
    set_tests_properties(package.consumer_configure PROPERTIES
        FIXTURES_REQUIRED package FIXTURES_SETUP consumer_configured)
    set_tests_properties(package.consumer_build PROPERTIES
        FIXTURES_REQUIRED consumer_configured FIXTURES_SETUP consumer_built)
    set_tests_properties(package.consumer_run PROPERTIES
        FIXTURES_REQUIRED consumer_built PASS_REGULAR_EXPRESSION "the session ended")
endif()

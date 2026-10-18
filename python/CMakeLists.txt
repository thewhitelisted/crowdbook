# The learning pool for Python: crowdbook::learning bound with nanobind, with the package around it
# laid out in this build directory, which imports as it is (PYTHONPATH=build/<preset>/python).
# Built when Python 3.12 or later is found with the files for building modules. A wheel's build
# (pyproject.toml, through scikit-build-core) builds it for Python's stable ABI, so that one module
# loads in 3.12 and every later version.
find_package(Python 3.12 COMPONENTS Interpreter Development.Module ${SKBUILD_SABI_COMPONENT})
if(NOT Python_FOUND)
    message(STATUS "Python 3.12 or later with its development files not found: the Python module "
                   "for learning is skipped")
    return()
endif()

include(FetchContent)
# nanobind's release from PyPI, whose source carries the robin-map headers that GitHub's archive
# of the tag leaves out.
FetchContent_Declare(nanobind
    URL https://files.pythonhosted.org/packages/c7/1f/e28fae715401e8f57400e72aeaff4fde12046d31828164f34f61f0816da8/nanobind-3.1.0.tar.gz
    URL_HASH SHA256=6661e3dc1434d78781ccf7913442d90361dab395c3424efadb63bbb6ddea7ea3
    SYSTEM)
FetchContent_MakeAvailable(nanobind)

# A module is a shared library, so the libraries linked into it must be position independent.
set_target_properties(crowdbook crowdbook_protocol crowdbook_scenario crowdbook_learning
    PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(package "${CMAKE_CURRENT_BINARY_DIR}/crowdbook_learn")
set(abi)
if(SKBUILD_SABI_COMPONENT)
    set(abi STABLE_ABI)
endif()
nanobind_add_module(crowdbook_learn_core NB_STATIC NB_SUPPRESS_WARNINGS ${abi} module.cpp)
set_target_properties(crowdbook_learn_core PROPERTIES
    OUTPUT_NAME _core
    LIBRARY_OUTPUT_DIRECTORY "${package}")
target_link_libraries(crowdbook_learn_core PRIVATE crowdbook::learning)
crowdbook_set_warnings(crowdbook_learn_core)
set(sources crowdbook_learn/__init__.py crowdbook_learn/evaluate.py)
foreach(source IN LISTS sources)
    configure_file(${source} "${CMAKE_CURRENT_BINARY_DIR}/${source}" COPYONLY)
endforeach()

# In a wheel, the package goes where Python finds it.
if(SKBUILD)
    install(TARGETS crowdbook_learn_core LIBRARY DESTINATION crowdbook_learn COMPONENT wheel)
    install(FILES ${sources} DESTINATION crowdbook_learn COMPONENT wheel)
endif()

if(CROWDBOOK_BUILD_TESTS)
    # The tests need NumPy; Gymnasium's are skipped without it.
    execute_process(COMMAND "${Python_EXECUTABLE}" -c "import numpy"
        RESULT_VARIABLE numpy OUTPUT_QUIET ERROR_QUIET)
    if(numpy EQUAL 0)
        # Run from the build directory, so that the package imported is the built one.
        add_test(NAME python.learning
            COMMAND "${Python_EXECUTABLE}" -m unittest discover -v
                -s "${CMAKE_CURRENT_SOURCE_DIR}/tests"
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        set(environment "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}"
            "CROWDBOOK_SOURCE_DIR=${PROJECT_SOURCE_DIR}")
        if(TARGET crowdbook_cli)
            list(APPEND environment "CROWDBOOK=$<TARGET_FILE:crowdbook_cli>")
        endif()
        set_tests_properties(python.learning PROPERTIES ENVIRONMENT "${environment}" TIMEOUT 600)
    else()
        message(STATUS "NumPy not found for ${Python_EXECUTABLE}: the learning module's tests are "
                       "skipped")
    endif()
endif()

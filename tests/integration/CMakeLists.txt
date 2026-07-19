add_test(
    NAME module_boundaries_reject_forbidden_edge
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/check_forbidden_dependency.py
        --cmake ${CMAKE_COMMAND}
        --checker ${PROJECT_SOURCE_DIR}/tools/check_module_boundaries.py
        --fixture ${CMAKE_CURRENT_SOURCE_DIR}/forbidden_dependency
        --build-dir ${PROJECT_BINARY_DIR}/tests/forbidden_dependency
)

add_test(
    NAME ugdr_environment_contract
    COMMAND
        ${Python3_EXECUTABLE}
        -m
        unittest
        tests.integration.test_ugdr_environment
)
set_tests_properties(
    ugdr_environment_contract
    PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
)

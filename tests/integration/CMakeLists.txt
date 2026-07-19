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
    NAME project_state_transition_matrix
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/check_project_state.py
        --python ${Python3_EXECUTABLE}
        --script ${PROJECT_SOURCE_DIR}/tools/project_state.py
        --schema ${PROJECT_SOURCE_DIR}/docs/status/current.schema.json
        --reviewed-doc ${PROJECT_SOURCE_DIR}/docs/v1_docs/F01_项目初始化与开发_Harness/F01-S02_项目文档、状态与进度交接体系_步骤文档.md
)

add_test(
    NAME project_docs_current_repository
    COMMAND
        ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tools/check_project_docs.py
        --root ${PROJECT_SOURCE_DIR}
)

add_test(
    NAME project_docs_reject_invalid_fixtures
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/check_project_docs_failures.py
        --python ${Python3_EXECUTABLE}
        --checker ${PROJECT_SOURCE_DIR}/tools/check_project_docs.py
        --schema ${PROJECT_SOURCE_DIR}/docs/status/current.schema.json
)

add_test(
    NAME project_handoff_surface
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/check_handoff_surface.py
        --agents ${PROJECT_SOURCE_DIR}/AGENTS.md
        --index ${PROJECT_SOURCE_DIR}/docs/v1_docs/README.md
        --state ${PROJECT_SOURCE_DIR}/docs/status/current.json
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

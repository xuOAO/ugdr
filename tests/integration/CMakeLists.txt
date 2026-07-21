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

add_executable(ugdr_ipc_client_server_test
    ipc_client_server_test.cpp
)
target_link_libraries(ugdr_ipc_client_server_test
    PRIVATE
        ugdr_ipc
)
add_test(
    NAME ugdr_ipc_client_server
    COMMAND ugdr_ipc_client_server_test
)

add_executable(ugdr_device_context_client_server_test
    device_context_client_server_test.cpp
)
target_link_libraries(ugdr_device_context_client_server_test
    PRIVATE
        ugdr_api
        ugdr_control
        ugdr_ipc
)
add_test(
    NAME ugdr_device_context_client_server
    COMMAND ugdr_device_context_client_server_test
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
    NAME client_contracts_current_repository
    COMMAND
        ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tools/check_client_contracts.py
        --root ${PROJECT_SOURCE_DIR}
)

add_test(
    NAME client_contracts_reject_invalid_fixtures
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/check_client_contract_failures.py
        --python ${Python3_EXECUTABLE}
        --checker ${PROJECT_SOURCE_DIR}/tools/check_client_contracts.py
        --repository ${PROJECT_SOURCE_DIR}
)

set_tests_properties(
    client_contracts_current_repository
    client_contracts_reject_invalid_fixtures
    PROPERTIES
        LABELS "ugdr_contract"
)

add_test(
    NAME project_handoff_surface
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/check_handoff_surface.py
        --agents ${PROJECT_SOURCE_DIR}/AGENTS.md
        --index ${PROJECT_SOURCE_DIR}/docs/v1_docs/README.md
        --state ${PROJECT_SOURCE_DIR}/docs/status/current.json
        --roadmap ${PROJECT_SOURCE_DIR}/docs/status/roadmap.json
)
set_tests_properties(
    project_handoff_surface
    PROPERTIES
        LABELS "ugdr_smoke"
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

add_test(
    NAME ugdr_quality_contract
    COMMAND
        ${Python3_EXECUTABLE}
        -m
        unittest
        tests.integration.test_ugdr_quality
)
set_tests_properties(
    ugdr_quality_contract
    PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
)

add_test(
    NAME ugdr_workflow_contract
    COMMAND
        ${Python3_EXECUTABLE}
        -m
        unittest
        tests.integration.test_ugdr_workflow
)
set_tests_properties(
    ugdr_workflow_contract
    PROPERTIES
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
)

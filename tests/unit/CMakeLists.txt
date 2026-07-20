add_test(
    NAME module_boundaries_positive
    COMMAND
        ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tools/check_module_boundaries.py
        --root ${PROJECT_SOURCE_DIR}
        --build-dir ${PROJECT_BINARY_DIR}
)

add_executable(ugdr_api_contract
    api_contract_test.cpp
)
target_link_libraries(ugdr_api_contract
    PRIVATE
        ugdr_api
)
target_compile_definitions(ugdr_api_contract
    PRIVATE
        UGDR_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
add_test(
    NAME ugdr_api_contract
    COMMAND ugdr_api_contract
)

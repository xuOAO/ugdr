add_test(
    NAME module_boundaries_positive
    COMMAND
        ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tools/check_module_boundaries.py
        --root ${PROJECT_SOURCE_DIR}
        --build-dir ${PROJECT_BINARY_DIR}
)

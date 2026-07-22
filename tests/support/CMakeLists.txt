add_library(ugdr_test_support STATIC
    mock_worker_fixture.cpp
)
target_include_directories(ugdr_test_support
    PUBLIC
        ${PROJECT_SOURCE_DIR}/tests
)
target_link_libraries(ugdr_test_support
    PUBLIC
        ugdr_api
        ugdr_queue
)

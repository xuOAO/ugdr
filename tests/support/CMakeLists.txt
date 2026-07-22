add_library(ugdr_test_support STATIC
    loop_worker_fixture.cpp
    mock_worker_fixture.cpp
    mock_gpu_backend.cpp
)
target_include_directories(ugdr_test_support
    PUBLIC
        ${PROJECT_SOURCE_DIR}/tests
)
target_link_libraries(ugdr_test_support
    PUBLIC
        ugdr_api
        ugdr_gpu
        ugdr_queue
        ugdr_worker
)

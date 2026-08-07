add_executable(ugdr_shared_ring_benchmark
    shared_ring_benchmark.cpp
)
target_link_libraries(ugdr_shared_ring_benchmark
    PRIVATE
        Threads::Threads
        ugdr_queue
)

add_executable(ugdr_wr_posting_benchmark
    wr_posting_benchmark.cpp
)
target_include_directories(ugdr_wr_posting_benchmark
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(ugdr_wr_posting_benchmark
    PRIVATE
        ugdr_api
        ugdr_queue
)

add_executable(ugdr_queue_metadata_benchmark
    queue_metadata_benchmark.cpp
)
target_include_directories(ugdr_queue_metadata_benchmark
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(ugdr_queue_metadata_benchmark
    PRIVATE
        ugdr_test_support
)

add_executable(ugdr_loop_worker_payload_benchmark
    loop_worker_payload_benchmark.cpp
    ${PROJECT_SOURCE_DIR}/src/api/wr_posting.cpp
)
target_include_directories(ugdr_loop_worker_payload_benchmark
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(ugdr_loop_worker_payload_benchmark
    PRIVATE
        ugdr_control
        ugdr_queue
        ugdr_worker
)

add_executable(ugdr_persistent_copy_benchmark
    persistent_copy_benchmark.cpp
)
target_include_directories(ugdr_persistent_copy_benchmark
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(ugdr_persistent_copy_benchmark
    PRIVATE
        ugdr_gpu
)

add_executable(ugdr_persistent_copy_latency_benchmark
    persistent_copy_latency_benchmark.cpp
)
target_include_directories(ugdr_persistent_copy_latency_benchmark
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(ugdr_persistent_copy_latency_benchmark
    PRIVATE
        ugdr_gpu
)

add_executable(ugdr_persistent_cuda_backend_benchmark
    persistent_cuda_backend_benchmark.cpp
    ${PROJECT_SOURCE_DIR}/src/api/wr_posting.cpp
)
target_include_directories(ugdr_persistent_cuda_backend_benchmark
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(ugdr_persistent_cuda_backend_benchmark
    PRIVATE
        ugdr_control
        ugdr_cuda_backend
        ugdr_queue
        ugdr_worker
)

add_executable(ugdr_client_daemon_cuda_benchmark
    client_daemon_cuda_benchmark.cpp
)
target_include_directories(ugdr_client_daemon_cuda_benchmark
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(ugdr_client_daemon_cuda_benchmark
    PRIVATE
        ugdr_api
        ugdr_control
        ugdr_cuda_backend
        ugdr_gpu
        ugdr_ipc
        ugdr_worker
)

add_test(
    NAME ugdr_client_daemon_cuda_benchmark_smoke
    COMMAND ugdr_client_daemon_cuda_benchmark
        --warmup-batches 1
        --bandwidth-batches 2
        --wr-bytes 65536
        --latency-warmup 1
        --latency-iterations 4
)
set_tests_properties(
    ugdr_client_daemon_cuda_benchmark_smoke
    PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 30
)

foreach(target
        ugdr_shared_ring_benchmark
        ugdr_wr_posting_benchmark
        ugdr_queue_metadata_benchmark
        ugdr_loop_worker_payload_benchmark
        ugdr_persistent_copy_benchmark
        ugdr_persistent_copy_latency_benchmark
        ugdr_persistent_cuda_backend_benchmark
        ugdr_client_daemon_cuda_benchmark)
    target_compile_definitions(${target}
        PRIVATE
            UGDR_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    )
endforeach()

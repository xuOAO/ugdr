cmake_minimum_required(VERSION 3.16)

project(UGDR VERSION 0.1.0 LANGUAGES C CXX CUDA)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
find_library(CUDA_DRIVER_LIBRARY NAMES cuda REQUIRED)

add_library(ugdr_queue STATIC
    src/queue/completion_queue.cpp
    src/queue/shared_ring.cpp
)
target_include_directories(ugdr_queue
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

add_library(ugdr_api STATIC
    src/api/api.cpp
    src/api/wr_posting.cpp
)
target_include_directories(ugdr_api
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(ugdr_api
    PRIVATE
        ugdr_control
        ugdr_gpu
        ugdr_queue
)

add_library(ugdr_ipc STATIC
    src/ipc/protocol.cpp
    src/ipc/client.cpp
    src/ipc/server.cpp
)
target_include_directories(ugdr_ipc
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

add_library(ugdr_control STATIC
    src/control/control.cpp
    src/control/device_context.cpp
    src/control/ipc_adapter.cpp
    src/control/object_identity.cpp
    src/control/pd_mr_cq.cpp
    src/control/qp.cpp
)
target_include_directories(ugdr_control
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(ugdr_control
    PRIVATE
        ugdr_ipc
        ugdr_queue
)

add_library(ugdr_worker STATIC
    src/worker/worker.cpp
)
target_include_directories(ugdr_worker
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(ugdr_worker
    PRIVATE
        ugdr_control
        ugdr_queue
)

add_library(ugdr_gpu STATIC
    src/gpu/cuda_ipc_memory.cu
    src/gpu/gpu.cu
)
target_include_directories(ugdr_gpu
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
        ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
)
target_link_libraries(ugdr_gpu
    PRIVATE
        ${CUDA_DRIVER_LIBRARY}
)

add_executable(ugdr_client
    apps/client/main.cpp
)
target_link_libraries(ugdr_client
    PRIVATE
        ugdr_api
)

add_executable(ugdr_daemon
    apps/daemon/main.cpp
)
target_include_directories(ugdr_daemon
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(ugdr_daemon
    PRIVATE
        ugdr_ipc
        ugdr_control
        ugdr_worker
        ugdr_gpu
)

enable_testing()
add_subdirectory(tests/unit)
add_subdirectory(tests/integration)
add_subdirectory(tests/smoke)

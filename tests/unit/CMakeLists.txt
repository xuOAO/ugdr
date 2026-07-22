add_test(
    NAME module_boundaries_positive
    COMMAND
        ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tools/check_module_boundaries.py
        --root ${PROJECT_SOURCE_DIR}
        --build-dir ${PROJECT_BINARY_DIR}
)

add_executable(ugdr_ipc_protocol_test
    ipc_protocol_test.cpp
)
target_link_libraries(ugdr_ipc_protocol_test
    PRIVATE
        ugdr_ipc
)
add_test(
    NAME ugdr_ipc_protocol
    COMMAND ugdr_ipc_protocol_test
)

add_executable(ugdr_ipc_adapter_test
    ipc_adapter_test.cpp
)
target_link_libraries(ugdr_ipc_adapter_test
    PRIVATE
        ugdr_control
)
add_test(
    NAME ugdr_ipc_adapter
    COMMAND ugdr_ipc_adapter_test
)

add_executable(ugdr_object_registry_test
    object_registry_test.cpp
)
target_link_libraries(ugdr_object_registry_test
    PRIVATE
        ugdr_control
)
add_test(
    NAME ugdr_object_registry
    COMMAND ugdr_object_registry_test
)

add_executable(ugdr_device_context_test
    device_context_test.cpp
)
target_link_libraries(ugdr_device_context_test
    PRIVATE
        ugdr_control
)
add_test(
    NAME ugdr_device_context
    COMMAND ugdr_device_context_test
)

add_executable(ugdr_pd_mr_cq_test
    pd_mr_cq_test.cpp
)
target_link_libraries(ugdr_pd_mr_cq_test
    PRIVATE
        ugdr_control
)
add_test(
    NAME ugdr_pd_mr_cq
    COMMAND ugdr_pd_mr_cq_test
)

add_executable(ugdr_qp_test
    qp_test.cpp
)
target_link_libraries(ugdr_qp_test
    PRIVATE
        ugdr_control
)
add_test(
    NAME ugdr_qp
    COMMAND ugdr_qp_test
)

add_executable(ugdr_shared_ring_test
    shared_ring_test.cpp
)
target_link_libraries(ugdr_shared_ring_test
    PRIVATE
        Threads::Threads
        ugdr_queue
)
add_test(
    NAME ugdr_shared_ring
    COMMAND ugdr_shared_ring_test
)

add_executable(ugdr_completion_queue_test
    completion_queue_test.cpp
)
target_link_libraries(ugdr_completion_queue_test
    PRIVATE
        ugdr_queue
)
add_test(
    NAME ugdr_completion_queue
    COMMAND ugdr_completion_queue_test
)

add_executable(ugdr_wr_posting_test
    wr_posting_test.cpp
)
target_include_directories(ugdr_wr_posting_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(ugdr_wr_posting_test
    PRIVATE
        ugdr_api
        ugdr_queue
)
add_test(
    NAME ugdr_wr_posting
    COMMAND ugdr_wr_posting_test
)

add_executable(ugdr_mock_worker_test
    mock_worker_test.cpp
)
target_include_directories(ugdr_mock_worker_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/tests
)
target_link_libraries(ugdr_mock_worker_test
    PRIVATE
        ugdr_test_support
)
add_test(
    NAME ugdr_mock_worker
    COMMAND ugdr_mock_worker_test
)

add_executable(ugdr_api_contract_c
    api_contract_c_test.c
)
set_property(TARGET ugdr_api_contract_c PROPERTY LINKER_LANGUAGE CXX)
target_link_libraries(ugdr_api_contract_c
    PRIVATE
        ugdr_api
)
add_test(
    NAME ugdr_api_contract_c
    COMMAND ugdr_api_contract_c
)

add_executable(ugdr_api_contract_cpp
    api_contract_test.cpp
)
target_link_libraries(ugdr_api_contract_cpp
    PRIVATE
        ugdr_api
)
add_test(
    NAME ugdr_api_contract_cpp
    COMMAND ugdr_api_contract_cpp
)

find_path(IBVERBS_INCLUDE_DIR infiniband/verbs.h)
if(NOT IBVERBS_INCLUDE_DIR)
    message(FATAL_ERROR
        "F02-S05 requires the rdma-core development header infiniband/verbs.h "
        "for Client contract alignment validation"
    )
endif()

add_executable(ugdr_libibverbs_alignment
    libibverbs_alignment_test.cpp
)
target_include_directories(ugdr_libibverbs_alignment
    PRIVATE
        ${IBVERBS_INCLUDE_DIR}
)
target_link_libraries(ugdr_libibverbs_alignment
    PRIVATE
        ugdr_api
)
add_test(
    NAME ugdr_libibverbs_alignment
    COMMAND ugdr_libibverbs_alignment
)

set_tests_properties(
    ugdr_api_contract_c
    ugdr_api_contract_cpp
    ugdr_libibverbs_alignment
    PROPERTIES
        LABELS "ugdr_contract"
)

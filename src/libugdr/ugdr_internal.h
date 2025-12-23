#pragma once
#include "../common/ipc/socket_utils.h"
#include "../common/ipc/shm_ring.h"
#include "../common/ugdr_types.h"

struct ugdr_context {
    ugdr::ipc::Socket sock;
    char dev_name[ugdr::common::UGDR_MAX_DEV_NAME_LEN];
};

struct ugdr_pd {
    ugdr::ipc::Socket* sock_ptr;
    uint32_t pd_handle;
};

struct ugdr_cq {
    ugdr::ipc::Socket* sock_ptr;
    ugdr::ipc::Shmem shmem;
    uint32_t cq_handle;
};

struct ugdr_qp {
    ugdr::ipc::Socket* sock_ptr;
    uint32_t pd_handle;
    uint32_t qp_handle;
    ugdr::ipc::Shmem rq;
    ugdr::ipc::Shmem sq;
    // TODO: 后续补充以下四个的功能
    void* qp_context;
    uint32_t max_sge;
    int qp_type;
    int sq_sig_all;
};
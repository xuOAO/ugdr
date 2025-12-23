#pragma once

#include <string>
#include <cstdint>
#include "../../common/ipc/shm_ring.h"

namespace ugdr{
namespace core{

struct shmring_attr {
    std::string ring_name;
    uint32_t ring_size;
    int fd = -1;
};

struct qp_init_attr {
    ipc::Shmem* send_cq;
    ipc::Shmem* recv_cq;

    uint32_t max_send_wr;
    uint32_t max_recv_wr;
    uint32_t max_sge;

    int qp_type;
    int sq_sig_all;
};

} // namespace core
} // namespace ugdr
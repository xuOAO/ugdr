#pragma once

#include <string>
#include <cstdint>

namespace ugdr{
namespace core{

struct shmring_attr {
    std::string ring_name;
    uint32_t ring_size;
    int fd;
};

struct qp_init_attr {
    uint32_t send_cq_handle;
    uint32_t recv_cq_handle;

    uint32_t max_send_wr;
    uint32_t max_recv_wr;
    uint32_t max_send_sge;

    int qp_type;
    int sq_sig_all;
};

} // namespace core
} // namespace ugdr
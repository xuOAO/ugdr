#include "qp.h"

namespace ugdr{
namespace core{

Qp::Qp(const struct qp_init_attr& qp_init_attr, uint32_t qp_handle) :
    send_cq_(qp_init_attr.send_cq), recv_cq_(qp_init_attr.recv_cq),
    max_send_wr_(qp_init_attr.max_send_wr), max_recv_wr_(qp_init_attr.max_recv_wr), max_sge_(qp_init_attr.max_sge), 
    qp_type_(qp_init_attr.qp_type), sq_sig_all_(qp_init_attr.sq_sig_all), qp_handle_(qp_handle)
{ 
    uint32_t sq_size = 64 * max_send_wr_;
    uint32_t rq_size = 64 * max_recv_wr_;
    sq_ = std::make_unique<ipc::Shmem>(std::string("sq_") + std::to_string(qp_handle), sq_size);
    rq_ = std::make_unique<ipc::Shmem>(std::string("rq_") + std::to_string(qp_handle), rq_size);
}

} // namespace core
} // namespace ugdr
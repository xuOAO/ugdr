#include "qp.h"

namespace ugdr{
namespace core{

Qp::Qp(Pd* pd, const struct qp_init_attr& qp_init_attr, uint32_t qp_handle) :
    pd_(pd), send_cq_(qp_init_attr.send_cq), recv_cq_(qp_init_attr.recv_cq),
    max_send_wr_(qp_init_attr.max_send_wr), max_recv_wr_(qp_init_attr.max_recv_wr), max_sge_(qp_init_attr.max_sge), 
    qp_type_(qp_init_attr.qp_type), sq_sig_all_(qp_init_attr.sq_sig_all), qp_handle_(qp_handle)
{ 
    std::string sq_name = std::string("sq_") + std::to_string(qp_handle);
    std::string rq_name = std::string("rq_") + std::to_string(qp_handle);
    sq_ = std::make_unique<ipc::SpscShmRing<common::Wqe>>(sq_name, max_send_wr_);
    rq_ = std::make_unique<ipc::SpscShmRing<common::Wqe>>(rq_name, max_recv_wr_);
}

} // namespace core
} // namespace ugdr
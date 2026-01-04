#pragma once
#include <cstdint>
#include <memory>
#include "../../common/ipc/shm.h"
#include "../../common/ipc/spsc_shmring.h"
#include "../../common/ugdr_types.h"
#include "daemon_types.h"

namespace ugdr{
namespace core{

class Pd;

class Qp {
public:
    //TODO: 12.22写这个
    Qp(Pd* pd, const struct qp_init_attr& qp_init_attr, uint32_t qp_handle);
    ~Qp() = default;

    inline Pd* get_pd() const { return pd_; }
    inline ipc::SpscShmRing<common::Wqe>* get_sq() const { return sq_.get(); }
    inline ipc::SpscShmRing<common::Wqe>* get_rq() const { return rq_.get(); }
    inline ipc::SpscShmRing<common::Cqe>* get_send_cq() const { return send_cq_; }
    inline ipc::SpscShmRing<common::Cqe>* get_recv_cq() const { return recv_cq_; }

private:
    std::unique_ptr<ipc::SpscShmRing<common::Wqe>> rq_; 
    std::unique_ptr<ipc::SpscShmRing<common::Wqe>> sq_;
    
    ipc::SpscShmRing<common::Cqe>* send_cq_;
    ipc::SpscShmRing<common::Cqe>* recv_cq_;

    uint32_t max_send_wr_;
    uint32_t max_recv_wr_;
    uint32_t max_sge_;

    int qp_type_;
    int sq_sig_all_;

    uint32_t qp_handle_; // for shring tail name
    Pd* pd_;
};

} // namespace core
} // mnamespace ugdr
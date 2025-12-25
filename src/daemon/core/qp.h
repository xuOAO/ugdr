#pragma once
#include <cstdint>
#include <memory>
#include "../../common/ipc/shm.h"
#include "daemon_types.h"

namespace ugdr{
namespace core{

class Qp {
public:
    //TODO: 12.22写这个
    Qp(const struct qp_init_attr& qp_init_attr, uint32_t qp_handle);
    ~Qp() = default;

    inline ipc::Shmem* get_sq() const { return rq_.get(); }
    inline ipc::Shmem* get_rq() const { return  sq_.get(); }

private:
    //TODO: 后续改成Shmring
    std::unique_ptr<ipc::Shmem> rq_; 
    std::unique_ptr<ipc::Shmem> sq_;
    
    ipc::Shmem* send_cq_;
    ipc::Shmem* recv_cq_;

    uint32_t max_send_wr_;
    uint32_t max_recv_wr_;
    uint32_t max_sge_;

    int qp_type_;
    int sq_sig_all_;

    uint32_t qp_handle_; // for shring tail name
};

} // namespace core
} // mnamespace ugdr
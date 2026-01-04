#include "pd.h"
#include "eth.h"
#include "mr_map.h"

namespace ugdr{
namespace core{

uint32_t Pd::create_qp(const struct qp_init_attr& qp_init_attr, struct shmring_attr* sq_attr, struct shmring_attr* rq_attr) {
    //TODO: 当前为简单的handle生成方式，后续需要更安全的处理
    // 1. set qp_handle
    static uint32_t next_qp_handle = 1;
    uint32_t qp_handle = next_qp_handle++;

    qp_map_[qp_handle] = std::make_unique<Qp>(this, qp_init_attr, qp_handle);
    Qp* qp = qp_map_[qp_handle].get();
    eth_->post_create_qp(qp);

    ipc::Shmem* sq = qp->get_sq();
    ipc::Shmem* rq = qp->get_rq();

    sq_attr->ring_name = sq->get_name();
    sq_attr->ring_size = sq->get_size();
    sq_attr->fd = sq->get_fd();
    rq_attr->ring_name = rq->get_name();
    rq_attr->ring_size = rq->get_size();
    rq_attr->fd = rq->get_fd();
    
    return qp_handle;
}

int Pd::destroy_qp(uint32_t qp_handle) {
    auto it = qp_map_.find(qp_handle);
    if (it != qp_map_.end()) {
        eth_->pre_destroy_qp(it->second.get());
        qp_map_.erase(it);
        return 0;
    } else {
        return -1;
    } 
}

Qp* Pd::get_qp(uint32_t qp_handle) {
    auto it = qp_map_.find(qp_handle);
    if (it != qp_map_.end()) {
        return it->second.get();
    } else {
        return nullptr;
    }
}


uint32_t Pd::create_mr(void* addr, size_t length, int access) {
    //TODO: 当前为简单的handle生成方式，后续需要更安全的处理
    // 1. set mr_handle
    // static uint32_t next_mr_handle = 1;
    // uint32_t mr_handle = next_mr_handle++;
    // mr_map_[mr_handle] = eth_->reg_mr(addr, length, access);
    Mr* mr = eth_->reg_mr(addr, length, access).release();
    uint32_t lkey = eth_->mr_map_->insert(mr);
    mr_map_[lkey] = std::unique_ptr<Mr>(mr);
    return lkey;
}

int Pd::destroy_mr(uint32_t lkey) {
    auto it = mr_map_.find(lkey);
    if (it != mr_map_.end()) {
        eth_->mr_map_->remove(lkey);
        mr_map_.erase(it);
        return 0;
    } else {
        return -1;
    }
}

Mr* Pd::get_mr(uint32_t lkey) {
    auto it = mr_map_.find(lkey);
    if (it != mr_map_.end()) {
        return it->second.get();
    } else {
        return nullptr;
    }
}

} // namespace core
} // namespace ugdr
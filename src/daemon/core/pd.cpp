#include "pd.h"

namespace ugdr{
namespace core{

uint32_t Pd::create_qp(const struct qp_init_attr& qp_init_attr, struct shmring_attr* sq_attr, struct shmring_attr* rq_attr) {
    //TODO: 当前为简单的handle生成方式，后续需要更安全的处理
    // 1. set qp_handle
    static uint32_t next_qp_handle = 1;
    uint32_t qp_handle = next_qp_handle++;

    qp_map[qp_handle] = std::make_unique<Qp>(qp_init_attr, qp_handle);
    ipc::Shmem* sq = qp_map[qp_handle]->get_sq();
    ipc::Shmem* rq = qp_map[qp_handle]->get_rq();

    sq_attr->ring_name = sq->get_name();
    sq_attr->ring_size = sq->get_size();
    sq_attr->fd = sq->get_fd();
    rq_attr->ring_name = rq->get_name();
    rq_attr->ring_size = rq->get_size();
    rq_attr->fd = rq->get_fd();
    
    return qp_handle;
}

int Pd::destroy_qp(uint32_t qp_handle) {
    auto it = qp_map.find(qp_handle);
    if (it != qp_map.end()) {
        qp_map.erase(it);
        return 0;
    } else {
        return -1;
    } 
}

Qp* Pd::get_qp(uint32_t qp_handle) {
    auto it = qp_map.find(qp_handle);
    if (it != qp_map.end()) {
        return it->second.get();
    } else {
        return nullptr;
    }
}

} // namespace core
} // namespace ugdr
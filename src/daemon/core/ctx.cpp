#include "ctx.h"
#include <memory>

namespace ugdr{
namespace core{

Ctx::Ctx(Eth* eth, const std::string& ctx_name) : eth_(eth), ctx_name_(ctx_name) {}

Ctx::~Ctx() = default;

uint32_t Ctx::alloc_pd() {
    //TODO: 当前为简单的handle生成方式，后续需更安全的处理
    static uint32_t next_pd_handle = 1; 
    uint32_t pd_handle = next_pd_handle++;
    pd_map_[pd_handle] = std::make_unique<Pd>(eth_);
    return pd_handle;
}

int Ctx::dealloc_pd(uint32_t pd_handle) {
    auto it = pd_map_.find(pd_handle);
    if (it != pd_map_.end()) {
        pd_map_.erase(it);
        return 0; // success
    } else {
        return -1; // pd_handle not found
    }
}

Pd* Ctx::get_pd(uint32_t pd_handle) {
    auto it = pd_map_.find(pd_handle);
    if (it != pd_map_.end()) {
        return it->second.get();
    } else {
        return nullptr;
    }
}

uint32_t Ctx::create_cq(uint32_t cqe, struct shmring_attr* shmring_attr) {
    //TODO: 当前为简单的handle生成方式，后续需更安全的处理
    // 1. set cq_handle
    static uint32_t next_cq_handle = 1;
    uint32_t cq_handle = next_cq_handle++;
    // 2. calculate minum shm size (this size will be aligned to page size in Shmem constructor)
    //TODO: 暂时设置为每个 cqe 64B，后续再定夺具体大小
    // size_t size = cqe * 64;
    // 3. create shmring and get fd
    //TODO: cq name 需要更规范一些
    std::string cq_name = ctx_name_ + "_cq_" + std::to_string(cq_handle);
    cq_map_[cq_handle] = std::make_unique<ipc::SpscShmRing<common::Cqe>>(cq_name, cqe);
    shmring_attr->ring_name = cq_map_[cq_handle]->get_name();
    shmring_attr->ring_size = cq_map_[cq_handle]->get_size();
    shmring_attr->fd = cq_map_[cq_handle]->get_fd();
    return cq_handle;
}

int Ctx::destroy_cq(uint32_t cq_handle) {
    auto it = cq_map_.find(cq_handle);
    if (it != cq_map_.end()) {
        cq_map_.erase(it);
        return 0; // success
    } else {
        return -1; // cq_handle not found
    }
}

ipc::SpscShmRing<common::Cqe>* Ctx::get_cq(uint32_t cq_handle) {
    auto it = cq_map_.find(cq_handle);
    if (it != cq_map_.end()) {
        return it->second.get();
    } else {
        return nullptr;
    }
}

} // namespace core
} // namespace ugdr
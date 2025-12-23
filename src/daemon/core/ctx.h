#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include "../utils/config.h"
#include "pd.h"
#include "../../common/ipc/shm_ring.h"
#include "daemon_types.h"

namespace ugdr{
namespace core{

class Ctx {
public:
    Ctx(const EthConfig& config);
    ~Ctx();

    // basic info
    std::string get_eth_name() const { return eth_name_; }
    // pd
    uint32_t alloc_pd();
    int dealloc_pd(uint32_t pd_handle);
    Pd* get_pd(uint32_t pd_handle);
    // cq
    uint32_t create_cq(uint32_t cqe, struct shmring_attr* shring_attr);
    int destroy_cq(uint32_t cq_handle);
    ipc::Shmem* get_cq(uint32_t cq_handle);
    //TODO: Shmem未来需要替换成 ShmRing类

private:
    std::string eth_name_;

    //TODO: 极其重要！！ 这里似乎有问题，目前close device只是释放socket，但是没有关闭相关的PD，如果没有显示调用dealloc_pd的情况，
    //那么pd以及起所包含的资源将不会释放，会造成严重的内存泄漏。
    std::unordered_map<uint32_t, std::unique_ptr<Pd>> pd_map_; // key: pd_handle, value: Pd pointer
    //TODO: Shmem未来需要替换成 ShmRing类
    std::unordered_map<uint32_t, std::unique_ptr<ipc::Shmem>> cq_map_; // key: cq_handle, value: shmring
};

}
}
#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include "../utils/config.h"
#include "pd.h"
#include "../../common/ipc/shm.h"
#include "../../common/ipc/spsc_shmring.h"
#include "../../common/ugdr_types.h"
#include "daemon_types.h"

namespace ugdr{
namespace core{

class Eth;

class Ctx {
public:
    Ctx(Eth* eth, const std::string& ctx_name);
    ~Ctx();

    //reverse link
    Eth* get_eth() const { return eth_; }

    // pd
    uint32_t alloc_pd();
    int dealloc_pd(uint32_t pd_handle);
    Pd* get_pd(uint32_t pd_handle);
    // cq
    uint32_t create_cq(uint32_t cqe, struct shmring_attr* shring_attr);
    int destroy_cq(uint32_t cq_handle);
    ipc::SpscShmRing<common::Cqe>* get_cq(uint32_t cq_handle);

private:
    Eth* eth_;
    std::string ctx_name_;

    std::unordered_map<uint32_t, std::unique_ptr<Pd>> pd_map_; // key: pd_handle, value: Pd pointer

    std::unordered_map<uint32_t, std::unique_ptr<ipc::SpscShmRing<common::Cqe>>> cq_map_; // key: cq_handle, value: shmring
};

}
}
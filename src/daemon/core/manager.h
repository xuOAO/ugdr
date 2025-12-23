#pragma once
#include <vector>
#include <memory>
#include <unordered_map>
#include "eth.h"
#include "../utils/config.h"
#include "ipc_server.h"

namespace ugdr{
namespace core{

class Manager {
public:
    Manager(const ManagerConfig& config);
    ~Manager();
    // void init();
    void run();
    Eth* get_eth(const std::string& eth_name);
    // uint32_t alloc_pd(uint32_t ctx_idx);
    // uint32_t dealloc_pd(uint32_t ctx_idx, uint32_t pd_idx);
private:
    ManagerConfig config_;
    std::unique_ptr<IpcServer> ipc_server_;

    uint32_t nb_eths_ = 0;
    std::vector<std::unique_ptr<Eth>> eths_;
    std::unordered_map<std::string, Eth*> dev_name_to_eth;
};

}
}
#include "../../common/logger.h"
#include "manager.h"

namespace ugdr{
namespace core{

// Manager::Manager(const ManagerConfig& config) : config_(config){}
Manager::Manager(const ManagerConfig& config) : config_(config){
    for (const auto& eth_config : config_.eth_configs){
        ctxs_.push_back(std::make_unique<Ctx>(eth_config));
        eth_name_to_ctx_idx_[eth_config.eth_name] = nb_ctxs_++;
    }

    ipc_server_ = std::make_unique<IpcServer>(config_.uds_path, this);
}

Manager::~Manager() = default;

// void Manager::init(){
//     for (const auto& eth_config : config_.eth_configs){
//         ctxs_.push_back(std::make_unique<Ctx>(eth_config));
//     }

//     ipc_server_ = std::make_unique<IpcServer>(config_.uds_path, this);

// }

void Manager::run(){
   UGDR_LOG_INFO("[Daemon] Listening on %s", config_.uds_path.c_str());
   ipc_server_->run_loop();
}

uint32_t Manager::get_ctx(const std::string& eth_name){
    auto it = eth_name_to_ctx_idx_.find(eth_name);
    if (it == eth_name_to_ctx_idx_.end()){
        return -1;
    }
    return it->second;
}

}
}
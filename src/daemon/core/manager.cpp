#include "../../common/logger.h"
#include "ctx.h"
#include "manager.h"

namespace ugdr{
namespace core{

// Manager::Manager(const ManagerConfig& config) : config_(config){}
Manager::Manager(const ManagerConfig& config) : config_(config){
    for (const auto& eth_config : config_.eth_configs){
        ctxs_.push_back(std::make_unique<Ctx>(eth_config));
        eth_to_ctx_[eth_config.eth_name] = ctxs_.back().get();
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

Ctx* Manager::get_ctx(const std::string& eth_name){
    auto it = eth_to_ctx_.find(eth_name);
    if (it == eth_to_ctx_.end()){
        return nullptr;
    }
    return it->second;
}

}
}
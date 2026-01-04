#include "../../common/logger.h"
#include "manager.h"
#include "../drivers/loopback/loopback_driver.h"

namespace ugdr{
namespace core{

// Manager::Manager(const ManagerConfig& config) : config_(config){}
Manager::Manager(const ManagerConfig& config) : config_(config){
    // for (const auto& eth_config : config_.eth_configs){
    //     eths_.push_back(std::make_unique<Eth>(eth_config));
    //     dev_name_to_eth[eth_config.eth_name] = eths_.back().get();
    // }
    if (config_.driver_type == "loopback"){
        driver_ = std::make_unique<loopback::LoopbackDriver>();
        driver_->init();
        eths_ = driver_->create_eths(config_.driver_config);
        for (const auto& eth_unique_ptr : eths_){
            dev_name_to_eth[eth_unique_ptr->get_eth_name()] = eth_unique_ptr.get();
        }
        workers_ = driver_->create_workers(config_.num_workers);
    } else {
        UGDR_LOG_ERROR("Unsupported driver type: %s", config_.driver_type.c_str());
        throw std::runtime_error("Unsupported driver type");
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
    UGDR_LOG_INFO("[Daemon] Starting %d workers", workers_.size());
    for(auto& worker : workers_){
        worker->start();
    }
    UGDR_LOG_INFO("[Daemon] Listening on %s", config_.uds_path.c_str());
    ipc_server_->run_loop();
}

Eth* Manager::get_eth(const std::string& eth_name){
    auto it = dev_name_to_eth.find(eth_name);
    if (it == dev_name_to_eth.end()){
        return nullptr;
    }
    return it->second;
}

}
}
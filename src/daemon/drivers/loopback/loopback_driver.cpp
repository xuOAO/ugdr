#include "loopback_driver.h"
#include "loopback_eth.h"
#include "loopback_worker.h"

namespace ugdr{
namespace loopback{

std::vector<LoopbackWorker*> LoopbackEth::workers_;

void LoopbackDriver::init() {
    // Initialization code for loopback driver if needed
}

std::vector<std::unique_ptr<core::Eth>> LoopbackDriver::create_eths(struct core::DriverConfig driver_config) {
    std::vector<std::unique_ptr<core::Eth>> eths;
    for (const auto& eth_config : driver_config.eth_configs){
        //TODO: 后续应为create loopback Eth
        eths.push_back(std::make_unique<LoopbackEth>(eth_config));
    }
    return eths;
}

std::vector<std::unique_ptr<core::Worker>> LoopbackDriver::create_workers(int num_workers) {
    std::vector<std::unique_ptr<core::Worker>> workers;
    for (int i = 0; i < num_workers; ++i) {
        auto worker = std::make_unique<LoopbackWorker>();
        LoopbackEth::add_worker(worker.get());
        workers.push_back(std::move(worker));
    }
    return workers;
}

// std::unique_ptr<core::Worker> LoopbackDriver::create_networker() {
//     auto worker = std::make_unique<LoopbackWorker>();
//     LoopbackEth::add_worker(worker.get());
//     return worker;
// }

// std::unique_ptr<core::Worker> LoopbackDriver::create_gpuworker() {
//     auto worker = std::make_unique<LoopbackWorker>();
//     LoopbackEth::add_worker(worker.get());
//     return worker;
// }

} // namespace loopback
} // namespace ugdr
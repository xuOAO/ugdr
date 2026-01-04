#pragma once
#include "../../core/driver.h"

namespace ugdr{
namespace loopback{

class LoopbackDriver : public core::Driver {
public:
    void init() override;
    std::vector<std::unique_ptr<core::Eth>> create_eths(struct core::DriverConfig driver_config) override;
    std::vector<std::unique_ptr<core::Worker>> create_workers(int num_workers) override;
    // std::unique_ptr<core::Worker> create_networker() override;
    // std::unique_ptr<core::Worker> create_gpuworker() override;
};

} // namespace loopback
} // namespace ugdr
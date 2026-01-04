#pragma once
#include <vector>
#include <memory>
#include "eth.h"
#include "worker.h"
// #include "mr.h"

namespace ugdr{
namespace core{

class Driver {
public:
    virtual ~Driver() = default;
    virtual void init() = 0;
    virtual std::vector<std::unique_ptr<Eth>> create_eths(struct DriverConfig driver_config) = 0;
    virtual std::vector<std::unique_ptr<Worker>> create_workers(int num_workers) = 0;
    // virtual std::unique_ptr<Worker> create_networker() = 0;
    // virtual std::unique_ptr<Worker> create_gpuworker() = 0;
};

} // namespace core
} // namespace ugdr
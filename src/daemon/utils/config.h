#pragma once
#include <string>
#include <vector>

namespace ugdr{
namespace core{

struct EthConfig {
    std::string eth_name;
};

struct DriverConfig {
    std::vector<EthConfig> eth_configs;
};

struct ManagerConfig {
    std::string driver_type;
    std::string uds_path;
    struct DriverConfig driver_config;
    //TODO: 后续要追加不同worker类，并且亲和numa
    int num_workers;
};

}
}
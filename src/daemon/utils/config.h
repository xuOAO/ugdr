#pragma once
#include <string>
#include <vector>

namespace ugdr{
namespace core{

struct EthConfig {
    std::string eth_name;
};

struct ManagerConfig {
    std::string uds_path;
    std::vector<EthConfig> eth_configs;
};

}
}
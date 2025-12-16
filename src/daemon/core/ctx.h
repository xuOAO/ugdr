#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include "../utils/config.h"
#include "pd.h"

namespace ugdr{
namespace core{

class Ctx {
public:
    Ctx(const EthConfig& config);
    ~Ctx();

    // pd
    uint32_t alloc_pd();
    int dealloc_pd(uint32_t pd_handle);
    Pd* get_pd(uint32_t pd_handle);

private:
    std::string eth_name_;

    std::unordered_map<uint32_t, std::unique_ptr<Pd>> pd_map_;
};

}
}
#pragma once

#include <string>
#include "../utils/config.h"

namespace ugdr{
namespace core{

class Ctx {
public:
    Ctx(const EthConfig& config);
    ~Ctx();
private:
    std::string eth_name_;
};

}
}
#pragma once
#include "ctx.h"

namespace ugdr{
namespace core{

//TODO: 将Eth插入到Manager和Ctx中间，Eth和Manager是静态配置的，Ctx是动态配置的
class Eth {
public:
    Eth(const EthConfig& config);
    ~Eth();

    std::string get_eth_name() const { return eth_name_; }
    Ctx* alloc_ctx(int client_fd);
    int dealloc_ctx(int client_fd);
    Ctx* get_ctx(int client_fd);

private:
    std::string eth_name_;
    std::unordered_map<std::uint32_t, std::unique_ptr<Ctx> > ctx_map_;

};

}
}
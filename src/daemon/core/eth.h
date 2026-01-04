#pragma once
#include "ctx.h"
#include "mr_map.h"

namespace ugdr{
namespace core{

class Qp;

//TODO: 将Eth插入到Manager和Ctx中间，Eth和Manager是静态配置的，Ctx是动态配置的
class Eth {
public:
    Eth(const EthConfig& config);
    virtual ~Eth();

    std::string get_eth_name() const { return eth_name_; }
    Ctx* alloc_ctx(int client_fd);
    int dealloc_ctx(int client_fd);
    Ctx* get_ctx(int client_fd);

    virtual std::unique_ptr<Mr> reg_mr(void* addr, size_t length, int access) = 0;
    virtual int dereg_mr(uint32_t pd_handle, uint32_t lkey) = 0;

    virtual void post_create_qp(Qp* qp) {}
    virtual void pre_destroy_qp(Qp* qp) {}

    std::unique_ptr<MrMap> mr_map_;

private:
    std::string eth_name_;
    std::unordered_map<std::uint32_t, std::unique_ptr<Ctx> > ctx_map_;
};

}
}
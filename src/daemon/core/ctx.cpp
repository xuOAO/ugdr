#pragma once

#include "ctx.h"

namespace ugdr{
namespace core{

Ctx::Ctx(const EthConfig& eth_config) : eth_name_{eth_config.eth_name} {}

Ctx::~Ctx() = default;

uint32_t Ctx::alloc_pd() {
    //TODO: 当前为简单的handle生成方式，后续需更安全的处理
    static uint32_t next_pd_handle = 1; 
    uint32_t pd_handle = next_pd_handle++;
    pd_map_[pd_handle] = std::make_unique<Pd>();
    return pd_handle;
}

int Ctx::dealloc_pd(uint32_t pd_handle) {
    auto it = pd_map_.find(pd_handle);
    if (it != pd_map_.end()) {
        pd_map_.erase(it);
        return 0; // success
    } else {
        return -1; // pd_handle not found
    }
}

Pd* Ctx::get_pd(uint32_t pd_handle) {
    auto it = pd_map_.find(pd_handle);
    if (it != pd_map_.end()) {
        return it->second.get();
    } else {
        return nullptr;
    }
}

} // namespace core
} // namespace ugdr
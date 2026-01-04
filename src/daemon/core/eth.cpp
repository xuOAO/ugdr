#include "eth.h"

namespace ugdr{
namespace core{

Eth::Eth(const EthConfig& eth_config) : eth_name_{eth_config.eth_name}, mr_map_(std::make_unique<MrMap>()) {}

Eth::~Eth() = default;

Ctx* Eth::alloc_ctx(int client_fd) {
    //TODO: 命名方式需要规范
    ctx_map_[client_fd] = std::make_unique<Ctx>(this, eth_name_ + std::to_string(client_fd));
    return ctx_map_[client_fd].get();
}

int Eth::dealloc_ctx(int client_fd) {
    auto it = ctx_map_.find(client_fd);
    if (it != ctx_map_.end()) {
        ctx_map_.erase(it);
        return 0;
    } else {
        return -1;
    }
}

Ctx* Eth::get_ctx(int client_fd) {
    auto it = ctx_map_.find(client_fd);
    if (it != ctx_map_.end()) {
        return it->second.get();
    } else {
        return nullptr;

    }
}

} // namespace core
} // namespace ugdr
#pragma once

#include "ctx.h"

namespace ugdr{
namespace core{

Ctx::Ctx(const EthConfig& eth_config) : eth_name_{eth_config.eth_name} {}

Ctx::~Ctx() = default;

}
}
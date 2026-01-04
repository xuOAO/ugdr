#pragma once
#include "../../core/mr.h"

namespace ugdr{
namespace loopback{

class LoopbackMr : public core::Mr {
public:
    LoopbackMr(void* addr, size_t length, int access)
        : core::Mr(addr, length, access) {}
    ~LoopbackMr() override = default;
};

} // namespace loopback
} // namespace ugdr
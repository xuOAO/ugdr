#pragma once
#include <cstdint>

namespace ugdr{
namespace core{

class Mr{
public:
    Mr(void* addr, size_t length, int access)
        : addr_(addr), length_(length), access_(access) {}
    virtual ~Mr() = default;

    void* get_addr() const { return addr_; }
    size_t get_length() const { return length_; }
    int get_access() const { return access_; }

private:
    void* addr_;
    size_t length_;
    int access_;
};

} // namespace core
} // namespace ugdr
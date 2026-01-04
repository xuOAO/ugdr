#pragma once
#include <cstdint>
#include <unordered_map>
#include <memory>
#include "qp.h"
#include "mr.h"
#include "daemon_types.h"

namespace ugdr{
namespace core{

class Eth;

class Pd {
public:
    Pd(Eth* eth) : eth_(eth) {}
    ~Pd() = default;

    uint32_t create_qp(const struct qp_init_attr& qp_init_attr, struct shmring_attr* sq_attr, struct shmring_attr* rq_attr);
    int destroy_qp(uint32_t qp_handle);
    Qp* get_qp(uint32_t qp_handle);

    uint32_t create_mr(void* addr, size_t length, int access);
    int destroy_mr(uint32_t lkey);
    Mr* get_mr(uint32_t lkey);

private:
    Eth* eth_;
    std::unordered_map<uint32_t, std::unique_ptr<Qp>> qp_map_;
    std::unordered_map<uint32_t, std::unique_ptr<Mr>> mr_map_;
};

}
}
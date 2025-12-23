#pragma once
#include <cstdint>
#include <unordered_map>
#include <memory>
#include "qp.h"
#include "daemon_types.h"

namespace ugdr{
namespace core{

class Pd {
public:
    Pd() = default;
    ~Pd() = default;

    uint32_t create_qp(const struct qp_init_attr& qp_init_attr, struct shmring_attr* sq_attr, struct shmring_attr* rq_attr);
    int destroy_qp(uint32_t qp_handle);
    Qp* get_qp(uint32_t qp_handle);

private:
    std::unordered_map<uint32_t, std::unique_ptr<Qp>> qp_map;
};

}
}
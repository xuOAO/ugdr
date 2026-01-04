#pragma once
#include "../../core/eth.h"
#include "../../core/mr.h"
#include "loopback_mr.h"
#include "loopback_worker.h"

namespace ugdr{
namespace loopback{

class LoopbackEth : public core::Eth {
public:
    LoopbackEth(const core::EthConfig& config) : core::Eth(config) {}
    ~LoopbackEth() override = default;

    std::unique_ptr<core::Mr> reg_mr(void* addr, size_t length, int access) override {
        // Loopback implementation of reg_mr
        // For simplicity, just return success
        return std::make_unique<LoopbackMr>(addr, length, access);
    }

    int dereg_mr(uint32_t pd_handle, uint32_t lkey) override {
        // Loopback implementation of dereg_mr
        // For simplicity, just return success
        return 0;
    }

    void post_create_qp(core::Qp* qp) override {
        if (workers_.empty()) return;
        static size_t next_worker_idx = 0;
        workers_[next_worker_idx]->add_qp(qp);
        next_worker_idx = (next_worker_idx + 1) % workers_.size();
    }

    void pre_destroy_qp(core::Qp* qp) override {
        for (auto* worker : workers_) {
            worker->remove_qp(qp);
        }
    }

    static void add_worker(LoopbackWorker* worker) {
        workers_.push_back(worker);
    }

private:
    static std::vector<LoopbackWorker*> workers_;
};

} // namespace loopback
} // namespace ugdr
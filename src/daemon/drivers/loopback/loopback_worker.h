#pragma once
#include "../../core/worker.h"
#include "../../core/qp.h"
#include "../../../common/ipc/spsc_shmring.h"
#include "../../../common/ugdr_types.h"
#include <vector>
#include <mutex>
#include <atomic>

namespace ugdr{
namespace core { class Pd; }
namespace loopback{

struct PollerItem {
    core::Qp* qp_ref;
    ipc::SpscShmRing<common::Wqe>* sq;
    ipc::SpscShmRing<common::Wqe>* rq;
    ipc::SpscShmRing<common::Cqe>* send_cq;
    ipc::SpscShmRing<common::Cqe>* recv_cq;
    core::Qp* peer_qp;
    core::Pd* pd; 
};

class LoopbackWorker : public core::Worker {
public:
    void add_qp(core::Qp* qp);
    void remove_qp(core::Qp* qp);

protected:
    void loop() override;

private:
    std::mutex mutex_;
    std::vector<PollerItem> shared_items_;
    std::atomic<bool> dirty_{false};
};

} // namespace loopback
} // namespace ugdr

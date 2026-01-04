#include "loopback_worker.h"
#include "../../core/pd.h"
#include "../../core/mr.h"

namespace ugdr {
namespace loopback {

void LoopbackWorker::add_qp(core::Qp* qp) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    PollerItem item;
    item.qp_ref = qp;
    item.sq = qp->get_sq();
    item.rq = qp->get_rq();
    item.send_cq = qp->get_send_cq();
    item.recv_cq = qp->get_recv_cq();
    // Slice 3: peer_qp points to self for loopback test
    item.peer_qp = qp; 
    item.pd = qp->get_pd(); 

    shared_items_.push_back(item);
    dirty_.store(true, std::memory_order_release);
}

void LoopbackWorker::remove_qp(core::Qp* qp) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = shared_items_.begin(); it != shared_items_.end(); it++) {
        if (it->qp_ref == qp) {
            it = shared_items_.erase(it);
            dirty_.store(true, std::memory_order_release);
            break;
        }
    }
}

void LoopbackWorker::loop() {
    std::vector<PollerItem> local_items;

    while (running_.load(std::memory_order_relaxed)) {
        // Control Plane Sync
        if (dirty_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(mutex_);
            local_items = shared_items_;
            dirty_.store(false, std::memory_order_release);
        }

        // Data Plane Polling
        bool work_done = false;
        constexpr int BATCH_SIZE = 32;
        common::Wqe wqes[BATCH_SIZE];
        common::Cqe cqes[BATCH_SIZE];

        for (auto& item : local_items) {
            int n = item.sq->pop_batch(wqes, BATCH_SIZE);
            if (n > 0) {
                work_done = true;
                int cqe_count = 0;

                for (int i = 0; i < n; ++i) {
                    common::Wqe& wqe = wqes[i];
                    common::Cqe& cqe = cqes[cqe_count++];
                    
                    // Initialize CQE
                    cqe = {}; 
                    cqe.wr_id = wqe.wr_id;
                    cqe.qp_num = wqe.qp_num;
                    cqe.opcode = wqe.opcode;

                    // Check MR
                    core::Mr* mr = item.pd->get_mr(wqe.sge.lkey);
                    if (!mr) {
                        cqe.status = common::WcStatus::LOC_PROT_ERR;
                        continue;
                    }

                    // Check bounds
                    uintptr_t wqe_addr = wqe.sge.addr;
                    uintptr_t mr_addr = (uintptr_t)mr->get_addr();
                    if (wqe_addr < mr_addr || wqe_addr + wqe.sge.length > mr_addr + mr->get_length()) {
                        cqe.status = common::WcStatus::LOC_ACCESS_ERR;
                        continue;
                    }

                    // Success
                    cqe.status = common::WcStatus::SUCCESS;
                    // Slice 3: Do-Nothing Strategy (Skip memcpy)
                }
                
                // Push batch to Send CQ
                int pushed = 0;
                while (pushed < cqe_count) {
                    int ret = item.send_cq->push_batch(cqes + pushed, cqe_count - pushed);
                    pushed += ret;
                    if (pushed < cqe_count) {
                        std::this_thread::yield();
                    }
                }
            }
        }

        if (!work_done) {
            // Avoid 100% CPU usage if idle, but keep latency low
            // For extreme performance testing, remove this yield
            std::this_thread::yield(); 
        }
    }
}

} // namespace loopback
} // namespace ugdr

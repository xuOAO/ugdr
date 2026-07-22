#pragma once

#include "queue/descriptors.hpp"
#include "queue/shared_ring.hpp"
#include "ugdr/api.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ugdr::test {

constexpr std::uint32_t kMockWorkerMaxSge = 4;

struct TrackerEvent {
    bool acquire = false;
    std::uint64_t wr_id = 0;
};

class LifecycleTracker {
  public:
    explicit LifecycleTracker(bool record_events = true);

    void reserve_events(std::size_t count);
    void acquire(std::uint64_t wr_id);
    void release(std::uint64_t wr_id);

    [[nodiscard]] bool balanced() const;
    [[nodiscard]] const std::vector<TrackerEvent> &events() const;

  private:
    int active_ = 0;
    bool record_events_ = true;
    std::vector<TrackerEvent> events_;
};

struct MockQp {
    queue::SharedRing sq;
    queue::SharedRing rq;
    queue::SharedRing *send_cq = nullptr;
    queue::SharedRing *recv_cq = nullptr;
    MockQp *peer = nullptr;
    std::uint32_t qp_num = 0;
    bool sq_sig_all = false;
    bool err = false;
};

bool make_ring(queue::QueueKind kind, std::uint32_t capacity, queue::SharedRing *ring);
bool make_qp(std::uint32_t qp_num, queue::SharedRing *send_cq, queue::SharedRing *recv_cq,
             MockQp *qp, std::uint32_t send_capacity = 8, std::uint32_t receive_capacity = 8);

class MockWorker {
  public:
    explicit MockWorker(LifecycleTracker *tracker);

    bool progress_once(MockQp &qp);
    void discard_remaining(MockQp &qp);

  private:
    bool flush_once(MockQp &qp);
    bool flush_slot(queue::SharedRing &work_queue, queue::SharedRing &completion_queue,
                    std::uint64_t wr_id, std::uint32_t qp_num);

    LifecycleTracker *tracker_;
};

bool post_send(MockQp &qp, std::uint64_t wr_id, ugdr_wr_opcode opcode, unsigned int flags,
               std::uint32_t immediate, const std::array<std::uint32_t, kMockWorkerMaxSge> &lengths,
               int num_sge, const void *payload = nullptr);
bool post_receive(MockQp &qp, std::uint64_t wr_id);

std::vector<queue::CompletionEntry> drain_cq(queue::SharedRing &ring);
int poll_completions(queue::SharedRing &ring, queue::CompletionEntry *entries,
                     std::uint32_t max_entries);
bool front_send_wr_id(queue::SharedRing &ring, std::uint64_t expected);
bool front_receive_wr_id(queue::SharedRing &ring, std::uint64_t expected);
bool ring_empty(queue::SharedRing &ring);
queue::CompletionEntry completion_marker(std::uint64_t wr_id);

}  // namespace ugdr::test

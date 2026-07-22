#include "support/mock_worker_fixture.hpp"

#include "api/wr_posting.hpp"
#include "queue/completion_queue.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ugdr::test {
namespace {

using queue::CompletionEntry;
using queue::ConstSlotBatch;
using queue::MutableSlotBatch;
using queue::ReceiveWqeHeader;
using queue::SendWqeHeader;
using queue::SharedRing;
using queue::SharedSge;

void *mutable_slot(const MutableSlotBatch &batch, std::uint32_t index, std::uint32_t stride) {
    if (index < batch.first.count) {
        return static_cast<std::byte *>(batch.first.data) +
               static_cast<std::size_t>(index) * stride;
    }
    return static_cast<std::byte *>(batch.second.data) +
           static_cast<std::size_t>(index - batch.first.count) * stride;
}

const void *const_slot(const ConstSlotBatch &batch, std::uint32_t index, std::uint32_t stride) {
    if (index < batch.first.count) {
        return static_cast<const std::byte *>(batch.first.data) +
               static_cast<std::size_t>(index) * stride;
    }
    return static_cast<const std::byte *>(batch.second.data) +
           static_cast<std::size_t>(index - batch.first.count) * stride;
}

struct CompletionReservation {
    SharedRing *ring = nullptr;
    std::array<CompletionEntry, 2> entries{};
    std::uint32_t count = 0;
    MutableSlotBatch slots{};
    bool reserved = false;
};

void append_completion(SharedRing *ring, const CompletionEntry &entry,
                       std::array<CompletionReservation, 2> *targets, std::uint32_t *target_count) {
    for (std::uint32_t index = 0; index < *target_count; ++index) {
        if ((*targets)[index].ring == ring) {
            (*targets)[index].entries[(*targets)[index].count++] = entry;
            return;
        }
    }
    CompletionReservation &target = (*targets)[(*target_count)++];
    target.ring = ring;
    target.entries[target.count++] = entry;
}

void cancel_all(std::array<CompletionReservation, 2> *targets, std::uint32_t target_count) {
    for (std::uint32_t index = 0; index < target_count; ++index) {
        CompletionReservation &target = (*targets)[index];
        if (target.reserved) {
            (void)target.ring->producer_publish(0);
            target.reserved = false;
        }
    }
}

bool reserve_all(std::array<CompletionReservation, 2> *targets, std::uint32_t target_count) {
    for (std::uint32_t index = 0; index < target_count; ++index) {
        CompletionReservation &target = (*targets)[index];
        const int status = target.ring->producer_reserve(target.count, &target.slots);
        if (status != 0) {
            cancel_all(targets, target_count);
            return false;
        }
        target.reserved = true;
        if (target.slots.count != target.count) {
            cancel_all(targets, target_count);
            return false;
        }
    }
    return true;
}

bool publish_all(std::array<CompletionReservation, 2> *targets, std::uint32_t target_count) {
    for (std::uint32_t index = 0; index < target_count; ++index) {
        CompletionReservation &target = (*targets)[index];
        for (std::uint32_t entry_index = 0; entry_index < target.count; ++entry_index) {
            std::memcpy(
                mutable_slot(target.slots, entry_index, target.ring->descriptor().slot_stride),
                &target.entries[entry_index], sizeof(CompletionEntry));
        }
    }
    for (std::uint32_t index = 0; index < target_count; ++index) {
        CompletionReservation &target = (*targets)[index];
        if (target.ring->producer_publish(target.count) != 0) {
            return false;
        }
        target.reserved = false;
    }
    return true;
}

std::uint32_t total_length(const SendWqeHeader &send) {
    const auto *sges = reinterpret_cast<const SharedSge *>(&send + 1);
    std::uint32_t total = 0;
    for (std::uint32_t index = 0; index < send.sge_count; ++index) {
        total += sges[index].length;
    }
    return total;
}

template <typename Header> void discard_ring(SharedRing &ring, LifecycleTracker *tracker) {
    const void *slot = nullptr;
    while (ring.consumer_peek(&slot) == 0) {
        const auto *header = static_cast<const Header *>(slot);
        const std::uint64_t wr_id = header->wr_id;
        tracker->acquire(wr_id);
        (void)ring.consumer_release();
        tracker->release(wr_id);
    }
}

template <typename Header> bool front_wr_id(SharedRing &ring, std::uint64_t expected) {
    const void *slot = nullptr;
    if (ring.consumer_peek(&slot) != 0) {
        return false;
    }
    const bool matches = static_cast<const Header *>(slot)->wr_id == expected;
    return ring.consumer_release(0) == 0 && matches;
}

}  // namespace

LifecycleTracker::LifecycleTracker(bool record_events) : record_events_(record_events) {
}

void LifecycleTracker::reserve_events(std::size_t count) {
    events_.reserve(count);
}

void LifecycleTracker::acquire(std::uint64_t wr_id) {
    ++active_;
    if (record_events_) {
        events_.push_back({true, wr_id});
    }
}

void LifecycleTracker::release(std::uint64_t wr_id) {
    --active_;
    if (record_events_) {
        events_.push_back({false, wr_id});
    }
}

bool LifecycleTracker::balanced() const {
    int active = 0;
    for (const TrackerEvent &event : events_) {
        active += event.acquire ? 1 : -1;
        if (active < 0) {
            return false;
        }
    }
    return active_ == 0 && (!record_events_ || active == 0);
}

const std::vector<TrackerEvent> &LifecycleTracker::events() const {
    return events_;
}

bool make_ring(queue::QueueKind kind, std::uint32_t capacity, SharedRing *ring) {
    std::uint32_t stride = queue::completion_slot_stride();
    if (kind == queue::QueueKind::send &&
        queue::send_slot_stride(kMockWorkerMaxSge, &stride) != 0) {
        return false;
    }
    if (kind == queue::QueueKind::receive &&
        queue::receive_slot_stride(kMockWorkerMaxSge, &stride) != 0) {
        return false;
    }
    return queue::create_shared_ring({kind, capacity, stride}, ring) == 0;
}

bool make_qp(std::uint32_t qp_num, SharedRing *send_cq, SharedRing *recv_cq, MockQp *qp,
             std::uint32_t send_capacity, std::uint32_t receive_capacity) {
    if (!make_ring(queue::QueueKind::send, send_capacity, &qp->sq) ||
        !make_ring(queue::QueueKind::receive, receive_capacity, &qp->rq)) {
        return false;
    }
    qp->send_cq = send_cq;
    qp->recv_cq = recv_cq;
    qp->qp_num = qp_num;
    return true;
}

MockWorker::MockWorker(LifecycleTracker *tracker) : tracker_(tracker) {
}

bool MockWorker::progress_once(MockQp &qp) {
    if (qp.err) {
        return flush_once(qp);
    }

    const void *send_slot = nullptr;
    if (qp.sq.consumer_peek(&send_slot) != 0) {
        return false;
    }
    const auto *send = static_cast<const SendWqeHeader *>(send_slot);
    const bool with_immediate = send->opcode == UGDR_WR_RDMA_WRITE_WITH_IMM;
    const ReceiveWqeHeader *receive = nullptr;
    if (with_immediate) {
        const void *receive_slot = nullptr;
        if (qp.peer == nullptr || qp.peer->rq.consumer_peek(&receive_slot) != 0) {
            (void)qp.sq.consumer_release(0);
            return false;
        }
        receive = static_cast<const ReceiveWqeHeader *>(receive_slot);
    }

    std::array<CompletionReservation, 2> completions{};
    std::uint32_t target_count = 0;
    if (qp.sq_sig_all || (send->send_flags & UGDR_SEND_SIGNALED) != 0) {
        CompletionEntry entry{};
        entry.wr_id = send->wr_id;
        entry.status = UGDR_WC_SUCCESS;
        entry.opcode = UGDR_WC_RDMA_WRITE;
        entry.qp_num = qp.qp_num;
        append_completion(qp.send_cq, entry, &completions, &target_count);
    }
    if (receive != nullptr) {
        CompletionEntry entry{};
        entry.wr_id = receive->wr_id;
        entry.status = UGDR_WC_SUCCESS;
        entry.opcode = UGDR_WC_RECV_RDMA_WITH_IMM;
        entry.byte_length = total_length(*send);
        entry.immediate_data = send->immediate_data;
        entry.qp_num = qp.peer->qp_num;
        entry.flags = UGDR_WC_WITH_IMM;
        append_completion(qp.peer->recv_cq, entry, &completions, &target_count);
    }

    if (!reserve_all(&completions, target_count)) {
        (void)qp.sq.consumer_release(0);
        if (receive != nullptr) {
            (void)qp.peer->rq.consumer_release(0);
        }
        return false;
    }

    tracker_->acquire(send->wr_id);
    if (receive != nullptr) {
        tracker_->acquire(receive->wr_id);
    }
    if (!publish_all(&completions, target_count)) {
        return false;
    }
    const std::uint64_t send_wr_id = send->wr_id;
    const std::uint64_t receive_wr_id = receive == nullptr ? 0 : receive->wr_id;
    (void)qp.sq.consumer_release();
    if (receive != nullptr) {
        (void)qp.peer->rq.consumer_release();
    }
    tracker_->release(send_wr_id);
    if (receive != nullptr) {
        tracker_->release(receive_wr_id);
    }
    return true;
}

void MockWorker::discard_remaining(MockQp &qp) {
    discard_ring<SendWqeHeader>(qp.sq, tracker_);
    discard_ring<ReceiveWqeHeader>(qp.rq, tracker_);
}

bool MockWorker::flush_once(MockQp &qp) {
    const void *slot = nullptr;
    if (qp.sq.consumer_peek(&slot) == 0) {
        const auto *send = static_cast<const SendWqeHeader *>(slot);
        return flush_slot(qp.sq, *qp.send_cq, send->wr_id, qp.qp_num);
    }
    if (qp.rq.consumer_peek(&slot) == 0) {
        const auto *receive = static_cast<const ReceiveWqeHeader *>(slot);
        return flush_slot(qp.rq, *qp.recv_cq, receive->wr_id, qp.qp_num);
    }
    return false;
}

bool MockWorker::flush_slot(SharedRing &work_queue, SharedRing &completion_queue,
                            std::uint64_t wr_id, std::uint32_t qp_num) {
    CompletionEntry entry{};
    entry.wr_id = wr_id;
    entry.status = UGDR_WC_WR_FLUSH_ERR;
    entry.qp_num = qp_num;
    std::array<CompletionReservation, 2> target{};
    target[0].ring = &completion_queue;
    target[0].entries[0] = entry;
    target[0].count = 1;
    if (!reserve_all(&target, 1)) {
        (void)work_queue.consumer_release(0);
        return false;
    }
    tracker_->acquire(wr_id);
    if (!publish_all(&target, 1)) {
        return false;
    }
    (void)work_queue.consumer_release();
    tracker_->release(wr_id);
    return true;
}

bool post_send(MockQp &qp, std::uint64_t wr_id, ugdr_wr_opcode opcode, unsigned int flags,
               std::uint32_t immediate, const std::array<std::uint32_t, kMockWorkerMaxSge> &lengths,
               int num_sge, const void *payload) {
    std::array<ugdr_sge, kMockWorkerMaxSge> sges{};
    for (int index = 0; index < num_sge; ++index) {
        const auto base = reinterpret_cast<std::uintptr_t>(payload);
        sges[static_cast<std::size_t>(index)] = {
            static_cast<std::uint64_t>(base + static_cast<std::uintptr_t>(index * 8)),
            lengths[static_cast<std::size_t>(index)], static_cast<std::uint32_t>(10 + index)};
    }
    ugdr_send_wr wr{};
    wr.wr_id = wr_id;
    wr.sg_list = num_sge == 0 ? nullptr : sges.data();
    wr.num_sge = num_sge;
    wr.opcode = opcode;
    wr.send_flags = flags;
    wr.imm_data = immediate;
    wr.wr.rdma.remote_addr = UINT64_C(0x100000);
    wr.wr.rdma.rkey = 7;
    ugdr_send_wr *bad_wr = nullptr;
    return api::post_send_chain(qp.sq, kMockWorkerMaxSge, &wr, &bad_wr) == 0;
}

bool post_receive(MockQp &qp, std::uint64_t wr_id) {
    ugdr_recv_wr wr{};
    wr.wr_id = wr_id;
    ugdr_recv_wr *bad_wr = nullptr;
    return api::post_receive_chain(qp.rq, kMockWorkerMaxSge, &wr, &bad_wr) == 0;
}

std::vector<CompletionEntry> drain_cq(SharedRing &ring) {
    ConstSlotBatch batch;
    if (ring.consumer_peek(ring.descriptor().capacity, &batch) == EAGAIN) {
        return {};
    }
    std::vector<CompletionEntry> entries(batch.count);
    for (std::uint32_t index = 0; index < batch.count; ++index) {
        std::memcpy(&entries[index], const_slot(batch, index, ring.descriptor().slot_stride),
                    sizeof(CompletionEntry));
    }
    (void)ring.consumer_release(batch.count);
    return entries;
}

int poll_completions(SharedRing &ring, CompletionEntry *entries, std::uint32_t max_entries) {
    if (entries == nullptr || max_entries == 0) {
        return EINVAL;
    }
    ConstSlotBatch batch;
    const int status = ring.consumer_peek(max_entries, &batch);
    if (status == EAGAIN) {
        return 0;
    }
    if (status != 0) {
        return -status;
    }
    for (std::uint32_t index = 0; index < batch.count; ++index) {
        std::memcpy(&entries[index], const_slot(batch, index, ring.descriptor().slot_stride),
                    sizeof(CompletionEntry));
    }
    return ring.consumer_release(batch.count) == 0 ? static_cast<int>(batch.count) : -EIO;
}

bool front_send_wr_id(SharedRing &ring, std::uint64_t expected) {
    return front_wr_id<SendWqeHeader>(ring, expected);
}

bool front_receive_wr_id(SharedRing &ring, std::uint64_t expected) {
    return front_wr_id<ReceiveWqeHeader>(ring, expected);
}

bool ring_empty(SharedRing &ring) {
    const void *slot = nullptr;
    return ring.consumer_peek(&slot) == EAGAIN;
}

CompletionEntry completion_marker(std::uint64_t wr_id) {
    CompletionEntry entry{};
    entry.wr_id = wr_id;
    entry.status = UGDR_WC_SUCCESS;
    entry.opcode = UGDR_WC_RDMA_WRITE;
    return entry;
}

}  // namespace ugdr::test

#include "api/wr_posting.hpp"
#include "queue/completion_queue.hpp"
#include "queue/descriptors.hpp"
#include "queue/shared_ring.hpp"
#include "ugdr/api.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

namespace {

using ugdr::queue::CompletionEntry;
using ugdr::queue::ConstSlotBatch;
using ugdr::queue::MutableSlotBatch;
using ugdr::queue::QueueDescriptor;
using ugdr::queue::QueueKind;
using ugdr::queue::ReceiveWqeHeader;
using ugdr::queue::SendWqeHeader;
using ugdr::queue::SharedRing;
using ugdr::queue::SharedSge;

constexpr std::uint32_t kMaxSge = 2;

struct TrackerEvent {
    bool acquire = false;
    std::uint64_t wr_id = 0;
};

class LifecycleTracker {
  public:
    void acquire(std::uint64_t wr_id) {
        ++active_;
        events_.push_back({true, wr_id});
    }

    void release(std::uint64_t wr_id) {
        --active_;
        events_.push_back({false, wr_id});
    }

    [[nodiscard]] bool balanced() const {
        int active = 0;
        for (const TrackerEvent &event : events_) {
            active += event.acquire ? 1 : -1;
            if (active < 0) {
                return false;
            }
        }
        return active == 0 && active_ == 0;
    }

    [[nodiscard]] const std::vector<TrackerEvent> &events() const {
        return events_;
    }

  private:
    int active_ = 0;
    std::vector<TrackerEvent> events_;
};

struct TestQp {
    SharedRing sq;
    SharedRing rq;
    SharedRing *send_cq = nullptr;
    SharedRing *recv_cq = nullptr;
    TestQp *peer = nullptr;
    std::uint32_t qp_num = 0;
    bool sq_sig_all = false;
    bool err = false;
};

bool make_ring(QueueKind kind, std::uint32_t capacity, SharedRing *ring) {
    std::uint32_t stride = ugdr::queue::completion_slot_stride();
    if (kind == QueueKind::send && ugdr::queue::send_slot_stride(kMaxSge, &stride) != 0) {
        return false;
    }
    if (kind == QueueKind::receive && ugdr::queue::receive_slot_stride(kMaxSge, &stride) != 0) {
        return false;
    }
    return ugdr::queue::create_shared_ring({kind, capacity, stride}, ring) == 0;
}

bool make_qp(std::uint32_t qp_num, SharedRing *send_cq, SharedRing *recv_cq, TestQp *qp) {
    if (!make_ring(QueueKind::send, 8, &qp->sq) || !make_ring(QueueKind::receive, 8, &qp->rq)) {
        return false;
    }
    qp->send_cq = send_cq;
    qp->recv_cq = recv_cq;
    qp->qp_num = qp_num;
    return true;
}

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

class MockWorker {
  public:
    explicit MockWorker(LifecycleTracker *tracker) : tracker_(tracker) {
    }

    bool progress_once(TestQp &qp) {
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

    void discard_remaining(TestQp &qp) {
        discard_ring<SendWqeHeader>(qp.sq);
        discard_ring<ReceiveWqeHeader>(qp.rq);
    }

  private:
    static void append_completion(SharedRing *ring, const CompletionEntry &entry,
                                  std::array<CompletionReservation, 2> *targets,
                                  std::uint32_t *target_count) {
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

    static void cancel_all(std::array<CompletionReservation, 2> *targets,
                           std::uint32_t target_count) {
        for (std::uint32_t index = 0; index < target_count; ++index) {
            CompletionReservation &target = (*targets)[index];
            if (target.reserved) {
                (void)target.ring->producer_publish(0);
                target.reserved = false;
            }
        }
    }

    static bool reserve_all(std::array<CompletionReservation, 2> *targets,
                            std::uint32_t target_count) {
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

    static bool publish_all(std::array<CompletionReservation, 2> *targets,
                            std::uint32_t target_count) {
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

    static std::uint32_t total_length(const SendWqeHeader &send) {
        const auto *sges = reinterpret_cast<const SharedSge *>(&send + 1);
        std::uint32_t total = 0;
        for (std::uint32_t index = 0; index < send.sge_count; ++index) {
            total += sges[index].length;
        }
        return total;
    }

    bool flush_once(TestQp &qp) {
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

    bool flush_slot(SharedRing &work_queue, SharedRing &completion_queue, std::uint64_t wr_id,
                    std::uint32_t qp_num) {
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

    template <typename Header> void discard_ring(SharedRing &ring) {
        const void *slot = nullptr;
        while (ring.consumer_peek(&slot) == 0) {
            const auto *header = static_cast<const Header *>(slot);
            const std::uint64_t wr_id = header->wr_id;
            tracker_->acquire(wr_id);
            (void)ring.consumer_release();
            tracker_->release(wr_id);
        }
    }

    LifecycleTracker *tracker_;
};

bool post_send(TestQp &qp, std::uint64_t wr_id, ugdr_wr_opcode opcode, unsigned int flags,
               std::uint32_t immediate, const std::array<std::uint32_t, 2> &lengths, int num_sge,
               const void *payload = nullptr) {
    std::array<ugdr_sge, 2> sges{};
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
    return ugdr::api::post_send_chain(qp.sq, kMaxSge, &wr, &bad_wr) == 0;
}

bool post_receive(TestQp &qp, std::uint64_t wr_id) {
    ugdr_recv_wr wr{};
    wr.wr_id = wr_id;
    ugdr_recv_wr *bad_wr = nullptr;
    return ugdr::api::post_receive_chain(qp.rq, kMaxSge, &wr, &bad_wr) == 0;
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

template <typename Header> bool front_wr_id(SharedRing &ring, std::uint64_t expected) {
    const void *slot = nullptr;
    if (ring.consumer_peek(&slot) != 0) {
        return false;
    }
    const bool matches = static_cast<const Header *>(slot)->wr_id == expected;
    return ring.consumer_release(0) == 0 && matches;
}

bool empty(SharedRing &ring) {
    const void *slot = nullptr;
    return ring.consumer_peek(&slot) == EAGAIN;
}

CompletionEntry marker(std::uint64_t wr_id) {
    CompletionEntry entry{};
    entry.wr_id = wr_id;
    entry.status = UGDR_WC_SUCCESS;
    entry.opcode = UGDR_WC_RDMA_WRITE;
    return entry;
}

bool ordinary_write_signaling_test() {
    SharedRing send_cq;
    SharedRing receive_cq;
    if (!make_ring(QueueKind::completion, 8, &send_cq) ||
        !make_ring(QueueKind::completion, 8, &receive_cq)) {
        return false;
    }
    TestQp requester;
    TestQp responder;
    if (!make_qp(11, &send_cq, &send_cq, &requester) ||
        !make_qp(22, &receive_cq, &receive_cq, &responder)) {
        return false;
    }
    requester.peer = &responder;
    responder.peer = &requester;
    LifecycleTracker tracker;
    MockWorker worker(&tracker);
    std::array<std::uint8_t, 32> payload{};
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<std::uint8_t>(index + 1);
    }
    const auto before = payload;

    if (!post_receive(responder, 90) ||
        !post_send(requester, 1, UGDR_WR_RDMA_WRITE, 0, 0, {16, 0}, 1, payload.data()) ||
        !worker.progress_once(requester) || !front_wr_id<ReceiveWqeHeader>(responder.rq, 90) ||
        !empty(send_cq)) {
        return false;
    }
    if (!post_send(requester, 2, UGDR_WR_RDMA_WRITE, UGDR_SEND_SIGNALED, 0, {8, 0}, 1,
                   payload.data()) ||
        !worker.progress_once(requester)) {
        return false;
    }
    requester.sq_sig_all = true;
    if (!post_send(requester, 3, UGDR_WR_RDMA_WRITE, 0, 0, {4, 0}, 1, payload.data()) ||
        !worker.progress_once(requester)) {
        return false;
    }
    const auto completions = drain_cq(send_cq);
    return payload == before && completions.size() == 2 && completions[0].wr_id == 2 &&
           completions[1].wr_id == 3 && completions[0].status == UGDR_WC_SUCCESS &&
           completions[0].opcode == UGDR_WC_RDMA_WRITE && completions[0].qp_num == 11 &&
           tracker.events().size() == 6 && tracker.balanced();
}

bool write_with_immediate_routing_test() {
    constexpr std::uint32_t immediate = UINT32_C(0x44332211);
    {
        SharedRing common_cq;
        if (!make_ring(QueueKind::completion, 4, &common_cq)) {
            return false;
        }
        TestQp requester;
        TestQp responder;
        if (!make_qp(31, &common_cq, &common_cq, &requester) ||
            !make_qp(32, &common_cq, &common_cq, &responder)) {
            return false;
        }
        requester.peer = &responder;
        LifecycleTracker tracker;
        MockWorker worker(&tracker);
        if (!post_receive(responder, 201) || !post_receive(responder, 202) ||
            !post_send(requester, 101, UGDR_WR_RDMA_WRITE_WITH_IMM, UGDR_SEND_SIGNALED, immediate,
                       {12, 20}, 2) ||
            !worker.progress_once(requester) || !front_wr_id<ReceiveWqeHeader>(responder.rq, 202)) {
            return false;
        }
        const auto completions = drain_cq(common_cq);
        if (completions.size() != 2 || completions[0].wr_id != 101 ||
            completions[0].opcode != UGDR_WC_RDMA_WRITE || completions[0].qp_num != 31 ||
            completions[1].wr_id != 201 || completions[1].opcode != UGDR_WC_RECV_RDMA_WITH_IMM ||
            completions[1].byte_length != 32 || completions[1].immediate_data != immediate ||
            completions[1].qp_num != 32 || completions[1].flags != UGDR_WC_WITH_IMM ||
            !tracker.balanced()) {
            return false;
        }
    }
    {
        SharedRing send_cq;
        SharedRing receive_cq;
        if (!make_ring(QueueKind::completion, 2, &send_cq) ||
            !make_ring(QueueKind::completion, 2, &receive_cq)) {
            return false;
        }
        TestQp requester;
        TestQp responder;
        if (!make_qp(41, &send_cq, &send_cq, &requester) ||
            !make_qp(42, &receive_cq, &receive_cq, &responder)) {
            return false;
        }
        requester.peer = &responder;
        LifecycleTracker tracker;
        MockWorker worker(&tracker);
        if (!post_receive(responder, 211) ||
            !post_send(requester, 111, UGDR_WR_RDMA_WRITE_WITH_IMM, UGDR_SEND_SIGNALED, immediate,
                       {7, 9}, 2) ||
            !worker.progress_once(requester)) {
            return false;
        }
        const auto sends = drain_cq(send_cq);
        const auto receives = drain_cq(receive_cq);
        if (sends.size() != 1 || sends[0].wr_id != 111 || receives.size() != 1 ||
            receives[0].wr_id != 211 || receives[0].byte_length != 16 ||
            receives[0].immediate_data != immediate || !tracker.balanced()) {
            return false;
        }
    }
    return true;
}

bool backpressure_test() {
    {
        SharedRing send_cq;
        SharedRing receive_cq;
        if (!make_ring(QueueKind::completion, 2, &send_cq) ||
            !make_ring(QueueKind::completion, 2, &receive_cq)) {
            return false;
        }
        TestQp requester;
        TestQp responder;
        if (!make_qp(51, &send_cq, &send_cq, &requester) ||
            !make_qp(52, &receive_cq, &receive_cq, &responder)) {
            return false;
        }
        requester.peer = &responder;
        LifecycleTracker tracker;
        MockWorker worker(&tracker);
        if (!post_send(requester, 301, UGDR_WR_RDMA_WRITE_WITH_IMM, 0, 7, {5, 0}, 1) ||
            worker.progress_once(requester) || !front_wr_id<SendWqeHeader>(requester.sq, 301) ||
            !empty(receive_cq) || !post_receive(responder, 401) ||
            !worker.progress_once(requester) || worker.progress_once(requester)) {
            return false;
        }
        const auto completions = drain_cq(receive_cq);
        if (completions.size() != 1 || completions[0].wr_id != 401 || !tracker.balanced()) {
            return false;
        }
    }
    {
        SharedRing send_cq;
        SharedRing receive_cq;
        if (!make_ring(QueueKind::completion, 1, &send_cq) ||
            !make_ring(QueueKind::completion, 1, &receive_cq)) {
            return false;
        }
        const CompletionEntry old = marker(999);
        if (ugdr::queue::produce_completions(receive_cq, &old, 1) != 1) {
            return false;
        }
        TestQp requester;
        TestQp responder;
        if (!make_qp(61, &send_cq, &send_cq, &requester) ||
            !make_qp(62, &receive_cq, &receive_cq, &responder)) {
            return false;
        }
        requester.peer = &responder;
        LifecycleTracker tracker;
        MockWorker worker(&tracker);
        if (!post_receive(responder, 501) ||
            !post_send(requester, 601, UGDR_WR_RDMA_WRITE_WITH_IMM, UGDR_SEND_SIGNALED, 9, {6, 0},
                       1) ||
            worker.progress_once(requester) || !empty(send_cq) ||
            !front_wr_id<SendWqeHeader>(requester.sq, 601) ||
            !front_wr_id<ReceiveWqeHeader>(responder.rq, 501)) {
            return false;
        }
        const auto existing = drain_cq(receive_cq);
        if (existing.size() != 1 || existing[0].wr_id != 999 || !worker.progress_once(requester) ||
            worker.progress_once(requester)) {
            return false;
        }
        const auto sends = drain_cq(send_cq);
        const auto receives = drain_cq(receive_cq);
        if (sends.size() != 1 || sends[0].wr_id != 601 || receives.size() != 1 ||
            receives[0].wr_id != 501 || !tracker.balanced()) {
            return false;
        }
    }
    return true;
}

bool err_flush_test() {
    SharedRing send_cq;
    SharedRing receive_cq;
    if (!make_ring(QueueKind::completion, 8, &send_cq) ||
        !make_ring(QueueKind::completion, 8, &receive_cq)) {
        return false;
    }
    const CompletionEntry old_send = marker(700);
    const CompletionEntry old_receive = marker(800);
    if (ugdr::queue::produce_completions(send_cq, &old_send, 1) != 1 ||
        ugdr::queue::produce_completions(receive_cq, &old_receive, 1) != 1) {
        return false;
    }
    TestQp qp;
    if (!make_qp(71, &send_cq, &receive_cq, &qp)) {
        return false;
    }
    LifecycleTracker tracker;
    MockWorker worker(&tracker);
    if (!post_send(qp, 701, UGDR_WR_RDMA_WRITE, 0, 0, {1, 0}, 1) ||
        !post_send(qp, 702, UGDR_WR_RDMA_WRITE, UGDR_SEND_SIGNALED, 0, {1, 0}, 1) ||
        !post_receive(qp, 801) || !post_receive(qp, 802)) {
        return false;
    }
    qp.err = true;
    if (!worker.progress_once(qp) || !worker.progress_once(qp) || !worker.progress_once(qp) ||
        !worker.progress_once(qp) || worker.progress_once(qp)) {
        return false;
    }
    const auto sends = drain_cq(send_cq);
    const auto receives = drain_cq(receive_cq);
    return sends.size() == 3 && sends[0].wr_id == 700 && sends[1].wr_id == 701 &&
           sends[2].wr_id == 702 && sends[1].status == UGDR_WC_WR_FLUSH_ERR &&
           sends[2].status == UGDR_WC_WR_FLUSH_ERR && receives.size() == 3 &&
           receives[0].wr_id == 800 && receives[1].wr_id == 801 && receives[2].wr_id == 802 &&
           receives[1].status == UGDR_WC_WR_FLUSH_ERR &&
           receives[2].status == UGDR_WC_WR_FLUSH_ERR && tracker.events().size() == 8 &&
           tracker.balanced();
}

bool teardown_test() {
    SharedRing send_cq;
    SharedRing receive_cq;
    if (!make_ring(QueueKind::completion, 2, &send_cq) ||
        !make_ring(QueueKind::completion, 2, &receive_cq)) {
        return false;
    }
    TestQp qp;
    if (!make_qp(81, &send_cq, &receive_cq, &qp)) {
        return false;
    }
    LifecycleTracker tracker;
    MockWorker worker(&tracker);
    if (!post_send(qp, 901, UGDR_WR_RDMA_WRITE, UGDR_SEND_SIGNALED, 0, {1, 0}, 1) ||
        !post_receive(qp, 902)) {
        return false;
    }
    worker.discard_remaining(qp);
    return empty(qp.sq) && empty(qp.rq) && empty(send_cq) && empty(receive_cq) &&
           tracker.events().size() == 4 && tracker.balanced();
}

}  // namespace

int main() {
    struct NamedTest {
        const char *name;
        bool (*run)();
    };
    const std::array tests{
        NamedTest{"ordinary write signaling", ordinary_write_signaling_test},
        NamedTest{"write with immediate routing", write_with_immediate_routing_test},
        NamedTest{"RQ and CQ backpressure", backpressure_test},
        NamedTest{"ERR flush", err_flush_test},
        NamedTest{"teardown", teardown_test},
    };
    for (const auto &[name, run] : tests) {
        if (!run()) {
            std::cerr << "mock worker test failed: " << name << '\n';
            return 1;
        }
    }
    return 0;
}

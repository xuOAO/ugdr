#include "support/mock_worker_fixture.hpp"

#include "queue/completion_queue.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using ugdr::queue::CompletionEntry;
using ugdr::queue::QueueKind;
using ugdr::queue::SharedRing;
using ugdr::test::completion_marker;
using ugdr::test::drain_cq;
using ugdr::test::front_receive_wr_id;
using ugdr::test::front_send_wr_id;
using ugdr::test::LifecycleTracker;
using ugdr::test::make_qp;
using ugdr::test::make_ring;
using ugdr::test::MockQp;
using ugdr::test::MockWorker;
using ugdr::test::post_receive;
using ugdr::test::post_send;
using ugdr::test::ring_empty;

bool ordinary_write_signaling_test() {
    SharedRing send_cq;
    SharedRing receive_cq;
    if (!make_ring(QueueKind::completion, 8, &send_cq) ||
        !make_ring(QueueKind::completion, 8, &receive_cq)) {
        return false;
    }
    MockQp requester;
    MockQp responder;
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
        !worker.progress_once(requester) || !front_receive_wr_id(responder.rq, 90) ||
        !ring_empty(send_cq)) {
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
        MockQp requester;
        MockQp responder;
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
            !worker.progress_once(requester) || !front_receive_wr_id(responder.rq, 202)) {
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
        MockQp requester;
        MockQp responder;
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
        MockQp requester;
        MockQp responder;
        if (!make_qp(51, &send_cq, &send_cq, &requester) ||
            !make_qp(52, &receive_cq, &receive_cq, &responder)) {
            return false;
        }
        requester.peer = &responder;
        LifecycleTracker tracker;
        MockWorker worker(&tracker);
        if (!post_send(requester, 301, UGDR_WR_RDMA_WRITE_WITH_IMM, 0, 7, {5, 0}, 1) ||
            worker.progress_once(requester) || !front_send_wr_id(requester.sq, 301) ||
            !ring_empty(receive_cq) || !post_receive(responder, 401) ||
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
        const CompletionEntry old = completion_marker(999);
        if (ugdr::queue::produce_completions(receive_cq, &old, 1) != 1) {
            return false;
        }
        MockQp requester;
        MockQp responder;
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
            worker.progress_once(requester) || !ring_empty(send_cq) ||
            !front_send_wr_id(requester.sq, 601) || !front_receive_wr_id(responder.rq, 501)) {
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
    const CompletionEntry old_send = completion_marker(700);
    const CompletionEntry old_receive = completion_marker(800);
    if (ugdr::queue::produce_completions(send_cq, &old_send, 1) != 1 ||
        ugdr::queue::produce_completions(receive_cq, &old_receive, 1) != 1) {
        return false;
    }
    MockQp qp;
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
    MockQp qp;
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
    return ring_empty(qp.sq) && ring_empty(qp.rq) && ring_empty(send_cq) &&
           ring_empty(receive_cq) && tracker.events().size() == 4 && tracker.balanced();
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

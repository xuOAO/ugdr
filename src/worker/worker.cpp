#include "worker/worker.hpp"

#include "queue/completion_queue.hpp"
#include "queue/descriptors.hpp"
#include "ugdr/api.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace ugdr::worker {
namespace {

const queue::SharedSge *send_sges(const queue::SendWqeHeader &send) noexcept {
    return reinterpret_cast<const queue::SharedSge *>(&send + 1);
}

std::uint32_t completion_status(DatagramResult result) noexcept {
    switch (result) {
    case DatagramResult::success:
        return UGDR_WC_SUCCESS;
    case DatagramResult::rnr:
        return UGDR_WC_RNR_RETRY_EXC_ERR;
    case DatagramResult::remote_invalid_request:
        return UGDR_WC_REM_INV_REQ_ERR;
    case DatagramResult::remote_access_error:
        return UGDR_WC_REM_ACCESS_ERR;
    case DatagramResult::remote_operation_error:
        return UGDR_WC_REM_OP_ERR;
    case DatagramResult::backend_error:
        return UGDR_WC_GENERAL_ERR;
    }
    return UGDR_WC_GENERAL_ERR;
}

queue::CompletionEntry send_completion(std::uint64_t wr_id, std::uint32_t qp_num,
                                       DatagramResult result) noexcept {
    queue::CompletionEntry entry{};
    entry.wr_id = wr_id;
    entry.status = completion_status(result);
    entry.opcode = UGDR_WC_RDMA_WRITE;
    entry.qp_num = qp_num;
    return entry;
}

}  // namespace

LoopWorker::LoopWorker(control::QpService &service, std::uint32_t qp_num, LocalTransport &transport,
                       CopyBackend &backend, LoopWorkerRole role)
    : service_(service), qp_num_(qp_num), transport_(transport), backend_(backend), role_(role) {
}

bool LoopWorker::progress_once() {
    control::WorkerQpView view;
    if (service_.worker_qp_view(qp_num_, &view) != 0) {
        return false;
    }
    bool progressed = false;
    if (role_ == LoopWorkerRole::responder) {
        progressed = try_backend_completion(view) || progressed;
        progressed = try_request(view) || progressed;
    } else {
        progressed = try_response(view) || progressed;
        progressed = try_send(view) || progressed;
    }
    return progressed;
}

bool LoopWorker::try_backend_completion(const control::WorkerQpView &view) {
    bool loaded = false;
    if (!pending_completion_.has_value()) {
        BackendCompletion completion;
        if (!backend_.try_pop_completion(completion)) {
            return false;
        }
        pending_completion_ = completion;
        loaded = true;
    }

    const auto inflight = responder_inflight_.find(pending_completion_->request_id);
    if (inflight == responder_inflight_.end()) {
        pending_completion_.reset();
        return true;
    }

    const ResponseDatagram response{pending_completion_->request_id, pending_completion_->result,
                                    0};
    void *receive_slot = nullptr;
    const bool produce_receive =
        pending_completion_->result == DatagramResult::success && inflight->second.has_receive;
    if (produce_receive) {
        const int reserve_status = view.receive_cq->producer_reserve(&receive_slot);
        if (reserve_status != 0) {
            return loaded;
        }
    }
    if (!transport_.try_push_response(response)) {
        if (produce_receive) {
            (void)view.receive_cq->producer_publish(0);
        }
        return loaded;
    }
    if (produce_receive) {
        queue::CompletionEntry entry{};
        entry.wr_id = inflight->second.receive_wr_id;
        entry.status = UGDR_WC_SUCCESS;
        entry.opcode = UGDR_WC_RECV_RDMA_WITH_IMM;
        entry.byte_length = inflight->second.byte_length;
        entry.immediate_data = inflight->second.immediate_data;
        entry.qp_num = view.qp_num;
        entry.flags = UGDR_WC_WITH_IMM;
        std::memcpy(receive_slot, &entry, sizeof(entry));
        if (view.receive_cq->producer_publish() != 0) {
            return false;
        }
    }
    responder_inflight_.erase(inflight);
    pending_completion_.reset();
    return true;
}

bool LoopWorker::try_response(const control::WorkerQpView &view) {
    bool loaded = false;
    if (!pending_response_.has_value()) {
        ResponseDatagram response;
        if (!transport_.try_pop_response(response)) {
            return false;
        }
        pending_response_ = response;
        loaded = true;
    }
    const auto inflight = requester_inflight_.find(pending_response_->request_id);
    if (inflight == requester_inflight_.end()) {
        pending_response_.reset();
        return true;
    }
    const bool needs_completion =
        pending_response_->result != DatagramResult::success || inflight->second.signaled;
    if (needs_completion) {
        const queue::CompletionEntry entry =
            send_completion(inflight->second.wr_id, view.qp_num, pending_response_->result);
        if (queue::produce_completions(*view.send_cq, &entry, 1) != 1) {
            return loaded;
        }
    }
    requester_inflight_.erase(inflight);
    pending_response_.reset();
    return true;
}

bool LoopWorker::try_request(const control::WorkerQpView &view) {
    bool loaded = false;
    if (!pending_request_.has_value()) {
        RequestDatagram request;
        if (!transport_.try_pop_request(request)) {
            return false;
        }
        pending_request_ = std::move(request);
        loaded = true;
    }
    RequestDatagram &request = *pending_request_;
    if (request.target_qp_num != view.qp_num) {
        if (!transport_.try_push_response(
                {request.request_id, DatagramResult::remote_invalid_request, 0})) {
            return loaded;
        }
        pending_request_.reset();
        return true;
    }

    std::uint64_t target_daemon_address = 0;
    if (service_.resolve_rkey(view.session_id, view.pd_identity, request.rkey,
                              request.remote_address, request.total_length,
                              &target_daemon_address) != 0) {
        if (!transport_.try_push_response(
                {request.request_id, DatagramResult::remote_access_error, 0})) {
            return loaded;
        }
        pending_request_.reset();
        return true;
    }

    const bool with_immediate = request.opcode == DatagramOpcode::rdma_write_with_immediate;
    const queue::ReceiveWqeHeader *receive = nullptr;
    if (with_immediate) {
        const void *receive_slot = nullptr;
        if (view.receive_queue->consumer_peek(&receive_slot) != 0) {
            return loaded;
        }
        receive = static_cast<const queue::ReceiveWqeHeader *>(receive_slot);
    }

    ResponderInflight metadata;
    metadata.has_receive = receive != nullptr;
    metadata.receive_wr_id = receive == nullptr ? 0 : receive->wr_id;
    metadata.immediate_data = request.immediate_data;
    metadata.byte_length = static_cast<std::uint32_t>(request.total_length);
    const auto [inflight, inserted] = responder_inflight_.emplace(request.request_id, metadata);
    if (!inserted) {
        if (receive != nullptr) {
            (void)view.receive_queue->consumer_release(0);
        }
        if (!transport_.try_push_response(
                {request.request_id, DatagramResult::remote_invalid_request, 0})) {
            return loaded;
        }
        pending_request_.reset();
        return true;
    }

    BackendRequest backend_request;
    backend_request.request_id = request.request_id;
    backend_request.source_segments = request.source_segments;
    backend_request.target_daemon_address = target_daemon_address;
    backend_request.total_length = request.total_length;
    if (!backend_.try_submit(backend_request)) {
        responder_inflight_.erase(inflight);
        if (receive != nullptr) {
            (void)view.receive_queue->consumer_release(0);
        }
        return loaded;
    }
    if (receive != nullptr && view.receive_queue->consumer_release() != 0) {
        return false;
    }
    pending_request_.reset();
    return true;
}

bool LoopWorker::try_send(const control::WorkerQpView &view) {
    const void *send_slot = nullptr;
    if (view.send_queue->consumer_peek(&send_slot) != 0) {
        return false;
    }
    const auto &send = *static_cast<const queue::SendWqeHeader *>(send_slot);
    if (send.sge_count > view.max_send_sge) {
        return complete_send_error(view, send, UGDR_WC_LOC_LEN_ERR);
    }
    if (send.opcode != UGDR_WR_RDMA_WRITE && send.opcode != UGDR_WR_RDMA_WRITE_WITH_IMM) {
        return complete_send_error(view, send, UGDR_WC_LOC_QP_OP_ERR);
    }

    RequestDatagram request;
    request.request_id = next_request_id();
    request.source_qp_num = view.qp_num;
    request.target_qp_num = view.peer_qp_num;
    request.opcode = send.opcode == UGDR_WR_RDMA_WRITE ? DatagramOpcode::rdma_write
                                                       : DatagramOpcode::rdma_write_with_immediate;
    request.remote_address = send.remote_address;
    request.rkey = send.rkey;
    request.immediate_data = send.immediate_data;
    request.source_segments.reserve(send.sge_count);
    const queue::SharedSge *const sges = send_sges(send);
    for (std::uint32_t index = 0; index < send.sge_count; ++index) {
        if (request.total_length > std::numeric_limits<std::uint32_t>::max() - sges[index].length) {
            return complete_send_error(view, send, UGDR_WC_LOC_LEN_ERR);
        }
        std::uint64_t daemon_address = 0;
        if (service_.resolve_lkey(view.session_id, view.pd_identity, sges[index].lkey,
                                  sges[index].address, sges[index].length, &daemon_address) != 0) {
            return complete_send_error(view, send, UGDR_WC_LOC_PROT_ERR);
        }
        request.total_length += sges[index].length;
        request.source_segments.push_back({daemon_address, sges[index].length});
    }

    const bool signaled = view.sq_sig_all != 0 || (send.send_flags & UGDR_SEND_SIGNALED) != 0;
    const auto [inflight, inserted] =
        requester_inflight_.emplace(request.request_id, RequesterInflight{send.wr_id, signaled});
    if (!inserted) {
        (void)view.send_queue->consumer_release(0);
        return false;
    }
    if (!transport_.try_push_request(request)) {
        requester_inflight_.erase(inflight);
        (void)view.send_queue->consumer_release(0);
        return false;
    }
    if (view.send_queue->consumer_release() != 0) {
        return false;
    }
    ++next_sequence_;
    if (next_sequence_ == 0) {
        next_sequence_ = 1;
    }
    return true;
}

bool LoopWorker::complete_send_error(const control::WorkerQpView &view,
                                     const queue::SendWqeHeader &send, std::uint32_t status) {
    queue::CompletionEntry entry{};
    entry.wr_id = send.wr_id;
    entry.status = status;
    entry.opcode = UGDR_WC_RDMA_WRITE;
    entry.qp_num = view.qp_num;
    if (queue::produce_completions(*view.send_cq, &entry, 1) != 1) {
        (void)view.send_queue->consumer_release(0);
        return false;
    }
    return view.send_queue->consumer_release() == 0;
}

std::uint64_t LoopWorker::next_request_id() noexcept {
    return (static_cast<std::uint64_t>(qp_num_) << 32U) | next_sequence_;
}

}  // namespace ugdr::worker

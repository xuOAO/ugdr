#include "worker/worker.hpp"

#include "queue/completion_queue.hpp"
#include "queue/descriptors.hpp"
#include "ugdr/api.hpp"

#include <algorithm>
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
                       CopyBackend &backend, LoopWorkerRole role, std::size_t payload_bytes,
                       ParentCompletionObserver *observer)
    : service_(service), qp_num_(qp_num), transport_(transport), backend_(backend), role_(role),
      payload_bytes_(payload_bytes == 0
                         ? kDefaultPayloadBytes
                         : std::min(payload_bytes, static_cast<std::size_t>(
                                                       std::numeric_limits<std::uint32_t>::max()))),
      observer_(observer) {
}

bool LoopWorker::progress_once() {
    control::WorkerQpView view;
    if (service_.worker_qp_view(qp_num_, &view) != 0) {
        return false;
    }
    bool progressed = false;
    if (role_ == LoopWorkerRole::responder) {
        progressed = try_backend_completion(view) || progressed;
        progressed = try_parent_response(view) || progressed;
        progressed = try_request(view) || progressed;
        progressed = try_parent_response(view) || progressed;
    } else {
        progressed = try_response(view) || progressed;
        progressed = try_send(view) || progressed;
    }
    return progressed;
}

bool LoopWorker::try_backend_completion(const control::WorkerQpView &) {
    BackendCompletion completion;
    if (!backend_.try_pop_completion(completion)) {
        return false;
    }

    const auto inflight = responder_inflight_.find(completion.parent_request_id);
    if (inflight == responder_inflight_.end()) {
        return true;
    }
    ResponderInflight &parent = inflight->second;
    if (completion.payload_index >= parent.payload_count ||
        parent.terminal[completion.payload_index] != 0) {
        parent.parent_error = DatagramResult::backend_error;
        return true;
    }
    parent.terminal[completion.payload_index] = 1;
    parent.results[completion.payload_index] = completion.result;
    ++parent.terminal_count;
    return true;
}

bool LoopWorker::try_parent_response(const control::WorkerQpView &view) {
    if (responder_order_.empty()) {
        return false;
    }
    const std::uint64_t parent_request_id = responder_order_.front();
    const auto inflight = responder_inflight_.find(parent_request_id);
    if (inflight == responder_inflight_.end()) {
        responder_order_.pop_front();
        return true;
    }
    ResponderInflight &parent = inflight->second;
    if (parent.next_payload_index != parent.payload_count ||
        parent.terminal_count != parent.payload_count) {
        return false;
    }

    DatagramResult result = parent.parent_error;
    if (result == DatagramResult::success) {
        for (const DatagramResult payload_result : parent.results) {
            if (payload_result != DatagramResult::success) {
                result = payload_result;
                break;
            }
        }
    }

    void *receive_slot = nullptr;
    const bool produce_receive = result == DatagramResult::success && parent.has_receive;
    if (produce_receive) {
        const int reserve_status = view.receive_cq->producer_reserve(&receive_slot);
        if (reserve_status != 0) {
            return false;
        }
    }
    if (!transport_.try_push_response({parent_request_id, result, 0})) {
        if (produce_receive) {
            (void)view.receive_cq->producer_publish(0);
        }
        return false;
    }
    if (produce_receive) {
        queue::CompletionEntry entry{};
        entry.wr_id = parent.receive_wr_id;
        entry.status = UGDR_WC_SUCCESS;
        entry.opcode = UGDR_WC_RECV_RDMA_WITH_IMM;
        entry.byte_length = parent.byte_length;
        entry.immediate_data = parent.immediate_data;
        entry.qp_num = view.qp_num;
        entry.flags = UGDR_WC_WITH_IMM;
        std::memcpy(receive_slot, &entry, sizeof(entry));
        if (view.receive_cq->producer_publish() != 0) {
            return false;
        }
    }

    responder_inflight_.erase(inflight);
    responder_order_.pop_front();
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
    const auto inflight = requester_inflight_.find(pending_response_->parent_request_id);
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
    if (observer_ != nullptr) {
        observer_->on_parent_completion({pending_response_->parent_request_id,
                                         inflight->second.wr_id, inflight->second.logical_bytes,
                                         inflight->second.payload_count,
                                         pending_response_->result});
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
        pending_request_ = request;
        loaded = true;
    }
    RequestDatagram &request = *pending_request_;
    if (request.target_qp_num != view.qp_num ||
        request.parent_total_length > std::numeric_limits<std::uint32_t>::max()) {
        if (!transport_.try_push_response(
                {request.parent_request_id, DatagramResult::remote_invalid_request, 0})) {
            return loaded;
        }
        pending_request_.reset();
        return true;
    }

    auto inflight = responder_inflight_.find(request.parent_request_id);
    if (inflight == responder_inflight_.end()) {
        const bool zero_parent = request.payload_count == 0;
        const bool valid_first =
            request.payload_index == 0 && request.payload_offset == 0 &&
            ((zero_parent && request.parent_total_length == 0 && request.payload_length == 0) ||
             (!zero_parent && request.parent_total_length != 0 && request.payload_length != 0 &&
              request.payload_length <= request.parent_total_length &&
              request.payload_count <= request.parent_total_length));
        if (!valid_first) {
            if (!transport_.try_push_response(
                    {request.parent_request_id, DatagramResult::remote_invalid_request, 0})) {
                return loaded;
            }
            pending_request_.reset();
            return true;
        }

        std::uint64_t target_daemon_address = 0;
        if (service_.resolve_rkey(view.session_id, view.pd_identity, request.rkey,
                                  request.remote_address, request.parent_total_length,
                                  &target_daemon_address) != 0) {
            if (!transport_.try_push_response(
                    {request.parent_request_id, DatagramResult::remote_access_error, 0})) {
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

        ResponderInflight parent;
        parent.source_qp_num = request.source_qp_num;
        parent.target_qp_num = request.target_qp_num;
        parent.opcode = request.opcode;
        parent.remote_address = request.remote_address;
        parent.rkey = request.rkey;
        parent.immediate_data = request.immediate_data;
        parent.parent_total_length = request.parent_total_length;
        parent.target_daemon_address = target_daemon_address;
        parent.payload_count = request.payload_count;
        parent.terminal.resize(request.payload_count, 0);
        parent.results.resize(request.payload_count, DatagramResult::success);
        parent.has_receive = receive != nullptr;
        parent.receive_wr_id = receive == nullptr ? 0 : receive->wr_id;
        parent.byte_length = static_cast<std::uint32_t>(request.parent_total_length);
        const auto inserted =
            responder_inflight_.emplace(request.parent_request_id, std::move(parent));
        if (!inserted.second) {
            return false;
        }
        responder_order_.push_back(request.parent_request_id);
        inflight = inserted.first;
    }

    ResponderInflight &parent = inflight->second;
    const bool zero_parent = parent.payload_count == 0;
    const bool valid_payload =
        request.source_qp_num == parent.source_qp_num &&
        request.target_qp_num == parent.target_qp_num && request.opcode == parent.opcode &&
        request.remote_address == parent.remote_address && request.rkey == parent.rkey &&
        request.immediate_data == parent.immediate_data &&
        request.parent_total_length == parent.parent_total_length &&
        request.payload_count == parent.payload_count &&
        ((zero_parent && request.payload_index == 0 && request.payload_offset == 0 &&
          request.payload_length == 0) ||
         (!zero_parent && request.payload_index == parent.next_payload_index &&
          request.payload_index < parent.payload_count &&
          request.payload_offset == parent.next_payload_offset && request.payload_length != 0 &&
          request.payload_offset <= parent.parent_total_length &&
          request.payload_length <= parent.parent_total_length - request.payload_offset));
    if (!valid_payload) {
        parent.parent_error = DatagramResult::remote_invalid_request;
        return loaded;
    }

    if (zero_parent) {
        if (parent.has_receive && !parent.receive_consumed) {
            if (view.receive_queue->consumer_release() != 0) {
                return false;
            }
            parent.receive_consumed = true;
        }
        pending_request_.reset();
        return true;
    }

    BackendRequest backend_request;
    backend_request.parent_request_id = request.parent_request_id;
    backend_request.parent_total_length = request.parent_total_length;
    backend_request.payload_offset = request.payload_offset;
    backend_request.source_daemon_address = request.source_daemon_address;
    backend_request.target_daemon_address = parent.target_daemon_address + request.payload_offset;
    backend_request.payload_length = request.payload_length;
    backend_request.payload_index = request.payload_index;
    backend_request.payload_count = request.payload_count;
    if (!backend_.try_submit(backend_request)) {
        return loaded;
    }
    if (parent.has_receive && !parent.receive_consumed) {
        if (view.receive_queue->consumer_release() != 0) {
            return false;
        }
        parent.receive_consumed = true;
    }
    ++parent.next_payload_index;
    parent.next_payload_offset += request.payload_length;
    if (parent.next_payload_index == parent.payload_count &&
        parent.next_payload_offset != parent.parent_total_length) {
        parent.parent_error = DatagramResult::remote_invalid_request;
    }
    pending_request_.reset();
    return true;
}

bool LoopWorker::try_send(const control::WorkerQpView &view) {
    if (!pending_send_.has_value()) {
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

        PendingSend parent;
        parent.parent_request_id = next_request_id();
        parent.wr_id = send.wr_id;
        parent.signaled = view.sq_sig_all != 0 || (send.send_flags & UGDR_SEND_SIGNALED) != 0;
        parent.opcode = send.opcode == UGDR_WR_RDMA_WRITE
                            ? DatagramOpcode::rdma_write
                            : DatagramOpcode::rdma_write_with_immediate;
        parent.source_qp_num = view.qp_num;
        parent.target_qp_num = view.peer_qp_num;
        parent.remote_address = send.remote_address;
        parent.rkey = send.rkey;
        parent.immediate_data = send.immediate_data;
        parent.source_segments.reserve(send.sge_count);

        std::uint64_t payload_count = 0;
        const queue::SharedSge *const sges = send_sges(send);
        for (std::uint32_t index = 0; index < send.sge_count; ++index) {
            if (parent.total_length >
                std::numeric_limits<std::uint32_t>::max() - sges[index].length) {
                return complete_send_error(view, send, UGDR_WC_LOC_LEN_ERR);
            }
            std::uint64_t daemon_address = 0;
            if (service_.resolve_lkey(view.session_id, view.pd_identity, sges[index].lkey,
                                      sges[index].address, sges[index].length,
                                      &daemon_address) != 0) {
                return complete_send_error(view, send, UGDR_WC_LOC_PROT_ERR);
            }
            parent.total_length += sges[index].length;
            if (sges[index].length != 0) {
                parent.source_segments.push_back({daemon_address, sges[index].length});
                payload_count +=
                    (static_cast<std::uint64_t>(sges[index].length) + payload_bytes_ - 1) /
                    payload_bytes_;
            }
        }
        if (payload_count > std::numeric_limits<std::uint32_t>::max()) {
            return complete_send_error(view, send, UGDR_WC_LOC_LEN_ERR);
        }
        parent.payload_count = static_cast<std::uint32_t>(payload_count);
        const auto inserted = requester_inflight_.emplace(
            parent.parent_request_id, RequesterInflight{parent.wr_id, parent.signaled,
                                                        parent.total_length, parent.payload_count});
        if (!inserted.second) {
            return false;
        }
        pending_send_ = std::move(parent);
    }

    PendingSend &parent = *pending_send_;
    RequestDatagram request;
    request.parent_request_id = parent.parent_request_id;
    request.source_qp_num = parent.source_qp_num;
    request.target_qp_num = parent.target_qp_num;
    request.opcode = parent.opcode;
    request.remote_address = parent.remote_address;
    request.rkey = parent.rkey;
    request.immediate_data = parent.immediate_data;
    request.parent_total_length = parent.total_length;
    request.payload_count = parent.payload_count;

    if (parent.total_length == 0) {
        if (parent.zero_payload_sent) {
            return false;
        }
    } else {
        const SourceSegment &segment = parent.source_segments[parent.segment_index];
        const std::uint32_t remaining = segment.length - parent.segment_offset;
        request.payload_length =
            static_cast<std::uint32_t>(std::min<std::size_t>(remaining, payload_bytes_));
        request.payload_index = parent.payload_index;
        request.payload_offset = parent.payload_offset;
        request.source_daemon_address = segment.daemon_address + parent.segment_offset;
    }

    if (!transport_.try_push_request(request)) {
        return false;
    }
    if (parent.total_length == 0) {
        parent.zero_payload_sent = true;
    } else {
        parent.segment_offset += request.payload_length;
        parent.payload_offset += request.payload_length;
        ++parent.payload_index;
        if (parent.segment_offset == parent.source_segments[parent.segment_index].length) {
            ++parent.segment_index;
            parent.segment_offset = 0;
        }
    }

    const bool finished = parent.total_length == 0 ? parent.zero_payload_sent
                                                   : parent.payload_index == parent.payload_count;
    if (!finished) {
        return true;
    }
    if (view.send_queue->consumer_release() != 0) {
        return false;
    }
    pending_send_.reset();
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

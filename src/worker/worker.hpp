#pragma once

#include "control/qp.hpp"
#include "queue/descriptors.hpp"
#include "worker/local_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ugdr::worker {

struct BackendRequest {
    std::uint64_t parent_request_id = 0;
    std::uint64_t parent_total_length = 0;
    std::uint64_t payload_offset = 0;
    std::uint64_t source_daemon_address = 0;
    std::uint64_t target_daemon_address = 0;
    std::uint32_t payload_length = 0;
    std::uint32_t payload_index = 0;
    std::uint32_t payload_count = 0;

    bool operator==(const BackendRequest &) const = default;
};

struct BackendCompletion {
    std::uint64_t parent_request_id = 0;
    std::uint32_t payload_index = 0;
    DatagramResult result = DatagramResult::success;

    bool operator==(const BackendCompletion &) const = default;
};

struct ParentCompletionEvent {
    std::uint64_t parent_request_id = 0;
    std::uint64_t wr_id = 0;
    std::uint64_t logical_bytes = 0;
    std::uint32_t payload_count = 0;
    DatagramResult result = DatagramResult::success;

    bool operator==(const ParentCompletionEvent &) const = default;
};

class ParentCompletionObserver {
  public:
    virtual ~ParentCompletionObserver() = default;
    virtual void on_parent_completion(const ParentCompletionEvent &event) noexcept = 0;
};

class CopyBackend {
  public:
    virtual ~CopyBackend() = default;

    virtual bool try_submit(const BackendRequest &request) = 0;
    virtual bool try_pop_completion(BackendCompletion &completion) = 0;
};

enum class LoopWorkerRole {
    requester,
    responder,
};

class LoopWorker {
  public:
    static constexpr std::size_t kDefaultPayloadBytes = 8192;

    LoopWorker(control::QpService &service, std::uint32_t qp_num, LocalTransport &transport,
               CopyBackend &backend, LoopWorkerRole role,
               std::size_t payload_bytes = kDefaultPayloadBytes,
               ParentCompletionObserver *observer = nullptr);

    bool progress_once();

  private:
    struct SourceSegment {
        std::uint64_t daemon_address = 0;
        std::uint32_t length = 0;
    };

    struct RequesterInflight {
        std::uint64_t wr_id = 0;
        bool signaled = false;
        std::uint64_t logical_bytes = 0;
        std::uint32_t payload_count = 0;
    };

    struct PendingSend {
        std::uint64_t parent_request_id = 0;
        std::uint64_t wr_id = 0;
        bool signaled = false;
        DatagramOpcode opcode = DatagramOpcode::rdma_write;
        std::uint32_t source_qp_num = 0;
        std::uint32_t target_qp_num = 0;
        std::uint64_t remote_address = 0;
        std::uint32_t rkey = 0;
        std::uint32_t immediate_data = 0;
        std::uint64_t total_length = 0;
        std::uint32_t payload_count = 0;
        std::vector<SourceSegment> source_segments;
        std::size_t segment_index = 0;
        std::uint32_t segment_offset = 0;
        std::uint64_t payload_offset = 0;
        std::uint32_t payload_index = 0;
        bool zero_payload_sent = false;
    };

    struct ResponderInflight {
        std::uint32_t source_qp_num = 0;
        std::uint32_t target_qp_num = 0;
        DatagramOpcode opcode = DatagramOpcode::rdma_write;
        std::uint64_t remote_address = 0;
        std::uint32_t rkey = 0;
        std::uint32_t immediate_data = 0;
        std::uint64_t parent_total_length = 0;
        std::uint64_t target_daemon_address = 0;
        std::uint64_t next_payload_offset = 0;
        std::uint32_t next_payload_index = 0;
        std::uint32_t payload_count = 0;
        std::uint32_t terminal_count = 0;
        std::vector<std::uint8_t> terminal;
        std::vector<DatagramResult> results;
        DatagramResult parent_error = DatagramResult::success;
        bool has_receive = false;
        bool receive_consumed = false;
        std::uint64_t receive_wr_id = 0;
        std::uint32_t byte_length = 0;
    };

    bool try_backend_completion(const control::WorkerQpView &view);
    bool try_parent_response(const control::WorkerQpView &view);
    bool try_response(const control::WorkerQpView &view);
    bool try_request(const control::WorkerQpView &view);
    bool try_send(const control::WorkerQpView &view);
    bool complete_send_error(const control::WorkerQpView &view, const queue::SendWqeHeader &send,
                             std::uint32_t status);
    std::uint64_t next_request_id() noexcept;

    control::QpService &service_;
    std::uint32_t qp_num_ = 0;
    LocalTransport &transport_;
    CopyBackend &backend_;
    LoopWorkerRole role_ = LoopWorkerRole::requester;
    std::size_t payload_bytes_ = kDefaultPayloadBytes;
    ParentCompletionObserver *observer_ = nullptr;
    std::uint32_t next_sequence_ = 1;
    std::unordered_map<std::uint64_t, RequesterInflight> requester_inflight_;
    std::unordered_map<std::uint64_t, ResponderInflight> responder_inflight_;
    std::deque<std::uint64_t> responder_order_;
    std::optional<PendingSend> pending_send_;
    std::optional<RequestDatagram> pending_request_;
    std::optional<ResponseDatagram> pending_response_;
};

}  // namespace ugdr::worker

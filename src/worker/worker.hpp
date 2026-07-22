#pragma once

#include "control/qp.hpp"
#include "queue/descriptors.hpp"
#include "worker/local_transport.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ugdr::worker {

struct BackendRequest {
    std::uint64_t request_id = 0;
    std::vector<ResolvedSourceSegment> source_segments;
    std::uint64_t target_daemon_address = 0;
    std::uint64_t total_length = 0;

    bool operator==(const BackendRequest &) const = default;
};

struct BackendCompletion {
    std::uint64_t request_id = 0;
    DatagramResult result = DatagramResult::success;

    bool operator==(const BackendCompletion &) const = default;
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
    LoopWorker(control::QpService &service, std::uint32_t qp_num, LocalTransport &transport,
               CopyBackend &backend, LoopWorkerRole role);

    bool progress_once();

  private:
    struct RequesterInflight {
        std::uint64_t wr_id = 0;
        bool signaled = false;
    };

    struct ResponderInflight {
        bool has_receive = false;
        std::uint64_t receive_wr_id = 0;
        std::uint32_t immediate_data = 0;
        std::uint32_t byte_length = 0;
    };

    bool try_backend_completion(const control::WorkerQpView &view);
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
    std::uint32_t next_sequence_ = 1;
    std::unordered_map<std::uint64_t, RequesterInflight> requester_inflight_;
    std::unordered_map<std::uint64_t, ResponderInflight> responder_inflight_;
    std::optional<RequestDatagram> pending_request_;
    std::optional<BackendCompletion> pending_completion_;
    std::optional<ResponseDatagram> pending_response_;
};

}  // namespace ugdr::worker

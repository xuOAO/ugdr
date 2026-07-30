#pragma once

#include "worker/local_transport.hpp"

#include <cstdint>

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

class CopyBackend {
  public:
    virtual ~CopyBackend() = default;

    virtual bool try_submit(const BackendRequest &request) = 0;
    virtual bool try_pop_completion(BackendCompletion &completion) = 0;
};

}  // namespace ugdr::worker

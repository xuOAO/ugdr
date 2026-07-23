#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

namespace ugdr::worker {

enum class DatagramOpcode {
    rdma_write,
    rdma_write_with_immediate,
};

enum class DatagramResult {
    success,
    rnr,
    remote_invalid_request,
    remote_access_error,
    remote_operation_error,
    backend_error,
};

struct RequestDatagram {
    std::uint64_t parent_request_id = 0;
    std::uint32_t source_qp_num = 0;
    std::uint32_t target_qp_num = 0;
    DatagramOpcode opcode = DatagramOpcode::rdma_write;
    std::uint64_t remote_address = 0;
    std::uint32_t rkey = 0;
    std::uint32_t immediate_data = 0;
    std::uint64_t parent_total_length = 0;
    std::uint64_t payload_offset = 0;
    std::uint64_t source_daemon_address = 0;
    std::uint32_t payload_length = 0;
    std::uint32_t payload_index = 0;
    std::uint32_t payload_count = 0;

    bool operator==(const RequestDatagram &) const = default;
};

struct ResponseDatagram {
    std::uint64_t parent_request_id = 0;
    DatagramResult result = DatagramResult::success;
    std::uint8_t rnr_delay = 0;

    bool operator==(const ResponseDatagram &) const = default;
};

class LocalTransport {
  public:
    LocalTransport(std::size_t request_capacity, std::size_t response_capacity);

    bool try_push_request(const RequestDatagram &request);
    bool try_pop_request(RequestDatagram &request);

    bool try_push_response(const ResponseDatagram &response);
    bool try_pop_response(ResponseDatagram &response);

  private:
    std::size_t request_capacity_ = 0;
    std::size_t response_capacity_ = 0;
    std::deque<RequestDatagram> requests_;
    std::deque<ResponseDatagram> responses_;
};

}  // namespace ugdr::worker

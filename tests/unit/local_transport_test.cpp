#include "worker/local_transport.hpp"

#include <array>
#include <cstdint>

namespace {

using ugdr::worker::DatagramOpcode;
using ugdr::worker::DatagramResult;
using ugdr::worker::LocalTransport;
using ugdr::worker::RequestDatagram;
using ugdr::worker::ResponseDatagram;

RequestDatagram request(std::uint64_t id, DatagramOpcode opcode = DatagramOpcode::rdma_write) {
    return {id,
            static_cast<std::uint32_t>(id + 10),
            static_cast<std::uint32_t>(id + 20),
            opcode,
            UINT64_C(0x100000000) + id,
            static_cast<std::uint32_t>(id + 30),
            static_cast<std::uint32_t>(id + 40),
            12,
            5,
            UINT64_C(0x200000000) + id,
            7,
            1,
            2};
}

ResponseDatagram response(std::uint64_t id, DatagramResult result) {
    const std::uint8_t rnr_delay = result == DatagramResult::rnr ? 19 : 0;
    return {id, result, rnr_delay};
}

bool request_round_trip_test() {
    LocalTransport transport(2, 1);
    const RequestDatagram expected = request(1, DatagramOpcode::rdma_write_with_immediate);
    RequestDatagram actual;
    return transport.try_push_request(expected) && transport.try_pop_request(actual) &&
           actual == expected;
}

bool response_round_trip_test() {
    constexpr std::array results{
        DatagramResult::success,
        DatagramResult::rnr,
        DatagramResult::remote_invalid_request,
        DatagramResult::remote_access_error,
        DatagramResult::remote_operation_error,
        DatagramResult::backend_error,
    };
    LocalTransport transport(1, results.size());
    for (std::size_t index = 0; index < results.size(); ++index) {
        if (!transport.try_push_response(response(index + 1, results[index]))) {
            return false;
        }
    }
    for (std::size_t index = 0; index < results.size(); ++index) {
        ResponseDatagram actual;
        const ResponseDatagram expected = response(index + 1, results[index]);
        if (!transport.try_pop_response(actual) || actual != expected) {
            return false;
        }
    }
    return true;
}

bool fifo_and_capacity_recovery_test() {
    LocalTransport transport(2, 2);
    const RequestDatagram first = request(1);
    const RequestDatagram second = request(2);
    const RequestDatagram third = request(3);
    RequestDatagram actual;
    return transport.try_push_request(first) && transport.try_push_request(second) &&
           !transport.try_push_request(third) && transport.try_pop_request(actual) &&
           actual == first && transport.try_push_request(third) &&
           transport.try_pop_request(actual) && actual == second &&
           transport.try_pop_request(actual) && actual == third &&
           !transport.try_pop_request(actual);
}

bool failure_has_no_side_effect_test() {
    LocalTransport transport(1, 1);
    const RequestDatagram accepted_request = request(11);
    const RequestDatagram rejected_request = request(12);
    RequestDatagram request_output = request(99);
    const RequestDatagram request_sentinel = request_output;
    if (!transport.try_push_request(accepted_request) ||
        transport.try_push_request(rejected_request)) {
        return false;
    }
    RequestDatagram actual_request;
    if (!transport.try_pop_request(actual_request) || actual_request != accepted_request ||
        transport.try_pop_request(request_output) || request_output != request_sentinel) {
        return false;
    }

    const ResponseDatagram accepted_response{21, DatagramResult::rnr, 7};
    const ResponseDatagram rejected_response{22, DatagramResult::backend_error, 0};
    ResponseDatagram response_output{99, DatagramResult::remote_operation_error, 0};
    const ResponseDatagram response_sentinel = response_output;
    if (!transport.try_push_response(accepted_response) ||
        transport.try_push_response(rejected_response)) {
        return false;
    }
    ResponseDatagram actual_response;
    return transport.try_pop_response(actual_response) && actual_response == accepted_response &&
           !transport.try_pop_response(response_output) && response_output == response_sentinel;
}

bool directions_are_independent_test() {
    LocalTransport transport(1, 1);
    const RequestDatagram first_request = request(31);
    const RequestDatagram second_request = request(32);
    const ResponseDatagram first_response{41, DatagramResult::success, 0};
    const ResponseDatagram second_response{42, DatagramResult::remote_access_error, 0};
    if (!transport.try_push_request(first_request) ||
        !transport.try_push_response(first_response)) {
        return false;
    }
    RequestDatagram actual_request;
    if (!transport.try_pop_request(actual_request) || actual_request != first_request ||
        !transport.try_push_request(second_request) ||
        transport.try_push_response(second_response)) {
        return false;
    }
    ResponseDatagram actual_response;
    return transport.try_pop_response(actual_response) && actual_response == first_response &&
           transport.try_push_response(second_response) &&
           transport.try_pop_request(actual_request) && actual_request == second_request &&
           transport.try_pop_response(actual_response) && actual_response == second_response;
}

}  // namespace

int main() {
    if (!request_round_trip_test()) {
        return 1;
    }
    if (!response_round_trip_test()) {
        return 2;
    }
    if (!fifo_and_capacity_recovery_test()) {
        return 3;
    }
    if (!failure_has_no_side_effect_test()) {
        return 4;
    }
    return directions_are_independent_test() ? 0 : 5;
}

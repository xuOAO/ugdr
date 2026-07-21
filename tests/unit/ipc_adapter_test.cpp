#include "control/ipc_adapter.hpp"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <iostream>
#include <vector>

namespace {

std::vector<std::byte> opaque(std::initializer_list<unsigned char> values) {
    std::vector<std::byte> bytes;
    bytes.reserve(values.size());
    for (const unsigned char value : values) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

}  // namespace

int main() {
    std::array<int, 2> descriptors{};
    if (::pipe2(descriptors.data(), O_CLOEXEC) != 0) {
        return 1;
    }
    ugdr::ipc::UniqueFd read_end(descriptors[0]);
    ugdr::ipc::UniqueFd write_end(descriptors[1]);

    ugdr::control::UgdrControlRequest request;
    request.method = 23;
    request.object_identity = UINT64_C(0x1122334455667788);
    request.length = UINT64_C(0x8877665544332211);
    request.access = UINT32_C(0xa5a5a5a5);
    request.opaque = opaque({0, 1, 2, 0xff});
    request.fd_indices = {0};

    std::vector<ugdr::ipc::UniqueFd> attached;
    attached.emplace_back(::dup(read_end.get()));
    ugdr::ipc::IpcMessage encoded;
    if (!attached.front().valid() ||
        ugdr::control::encode_request(request, std::move(attached), &encoded) != 0) {
        return 2;
    }
    ugdr::control::DecodedControlRequest decoded;
    const int decode_status = ugdr::control::decode_request(std::move(encoded), &decoded);
    if (decode_status != 0) {
        std::cerr << "request decode failed: " << decode_status << '\n';
        return 3;
    }
    if (decoded.value.method != request.method ||
        decoded.value.object_identity != request.object_identity ||
        decoded.value.length != request.length || decoded.value.access != request.access ||
        decoded.value.opaque != request.opaque || decoded.value.fd_indices != request.fd_indices ||
        decoded.file_descriptors.size() != 1) {
        std::cerr << "request round-trip mismatch\n";
        return 9;
    }

    ugdr::control::UgdrControlResponse response;
    response.method = 29;
    response.status = EOPNOTSUPP;
    response.object_identity = UINT64_C(0xfedcba9876543210);
    response.opaque = opaque({9, 8, 7});
    response.fd_indices = {0};
    std::vector<ugdr::ipc::UniqueFd> response_fds;
    response_fds.emplace_back(::dup(write_end.get()));
    if (ugdr::control::encode_response(response, std::move(response_fds), &encoded) != 0) {
        return 4;
    }
    ugdr::control::DecodedControlResponse decoded_response;
    if (ugdr::control::decode_response(std::move(encoded), &decoded_response) != 0 ||
        decoded_response.value.method != response.method ||
        decoded_response.value.status != response.status ||
        decoded_response.value.object_identity != response.object_identity ||
        decoded_response.value.opaque != response.opaque ||
        decoded_response.value.fd_indices != response.fd_indices ||
        decoded_response.file_descriptors.size() != 1) {
        return 5;
    }

    request.fd_indices = {1};
    attached.clear();
    attached.emplace_back(::dup(read_end.get()));
    if (ugdr::control::encode_request(request, std::move(attached), &encoded) != -EINVAL) {
        return 6;
    }

    request.fd_indices.clear();
    if (ugdr::control::encode_request(request, {}, &encoded) != 0) {
        return 7;
    }
    encoded.payload.pop_back();
    return ugdr::control::decode_request(std::move(encoded), &decoded) == -EPROTO ? 0 : 8;
}

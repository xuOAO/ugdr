#pragma once

#include "ipc/ipc.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ugdr::control {

constexpr std::uint16_t kControlPayloadVersion = 1;

struct UgdrControlRequest {
    std::uint32_t method = 0;
    std::uint64_t object_identity = 0;
    std::uint64_t length = 0;
    std::uint32_t access = 0;
    std::vector<std::byte> opaque;
    std::vector<std::uint32_t> fd_indices;
};

struct UgdrControlResponse {
    std::uint32_t method = 0;
    std::int32_t status = 0;
    std::uint64_t object_identity = 0;
    std::vector<std::byte> opaque;
    std::vector<std::uint32_t> fd_indices;
};

struct DecodedControlRequest {
    UgdrControlRequest value;
    std::vector<ipc::UniqueFd> file_descriptors;
};

struct DecodedControlResponse {
    UgdrControlResponse value;
    std::vector<ipc::UniqueFd> file_descriptors;
};

int encode_request(const UgdrControlRequest &request, std::vector<ipc::UniqueFd> file_descriptors,
                   ipc::IpcMessage *message);
int decode_request(ipc::IpcMessage message, DecodedControlRequest *request);
int encode_response(const UgdrControlResponse &response,
                    std::vector<ipc::UniqueFd> file_descriptors, ipc::IpcMessage *message);
int decode_response(ipc::IpcMessage message, DecodedControlResponse *response);

}  // namespace ugdr::control

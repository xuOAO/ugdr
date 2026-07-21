#include "control/ipc_adapter.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <limits>

#include <utility>

namespace ugdr::control {
namespace {

constexpr std::size_t kRequestFixedSize = 32;
constexpr std::size_t kResponseFixedSize = 20;

std::uint64_t host_to_network64(std::uint64_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (static_cast<std::uint64_t>(htonl(static_cast<std::uint32_t>(value))) << 32U) |
           htonl(static_cast<std::uint32_t>(value >> 32U));
#else
    return value;
#endif
}

std::uint64_t network_to_host64(std::uint64_t value) noexcept {
    return host_to_network64(value);
}

template <typename T> void append(std::vector<std::byte> *payload, T value) {
    const auto *const bytes = reinterpret_cast<const std::byte *>(&value);
    payload->insert(payload->end(), bytes, bytes + sizeof(value));
}

template <typename T>
bool read(const std::vector<std::byte> &payload, std::size_t *offset, T *value) {
    if (*offset > payload.size() || payload.size() - *offset < sizeof(T)) {
        return false;
    }
    std::memcpy(value, payload.data() + *offset, sizeof(T));
    *offset += sizeof(T);
    return true;
}

bool valid_indices(const std::vector<std::uint32_t> &indices, std::size_t descriptor_count) {
    for (const std::uint32_t index : indices) {
        if (index >= descriptor_count) {
            return false;
        }
    }
    return true;
}

int validate_lengths(std::size_t opaque_size, std::size_t index_count, std::size_t fixed_size) {
    if (opaque_size > std::numeric_limits<std::uint32_t>::max() ||
        index_count > std::numeric_limits<std::uint32_t>::max() ||
        index_count > (ipc::kMaxPayloadSize - fixed_size) / sizeof(std::uint32_t)) {
        return -EMSGSIZE;
    }
    const std::size_t index_bytes = index_count * sizeof(std::uint32_t);
    if (opaque_size > ipc::kMaxPayloadSize - fixed_size - index_bytes) {
        return -EMSGSIZE;
    }
    return 0;
}

void append_indices(std::vector<std::byte> *payload, const std::vector<std::uint32_t> &indices) {
    for (const std::uint32_t index : indices) {
        append(payload, htonl(index));
    }
}

bool read_indices(const std::vector<std::byte> &payload, std::size_t *offset, std::uint32_t count,
                  std::vector<std::uint32_t> *indices) {
    indices->reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t wire_value = 0;
        if (!read(payload, offset, &wire_value)) {
            return false;
        }
        indices->push_back(ntohl(wire_value));
    }
    return true;
}

}  // namespace

int encode_request(const UgdrControlRequest &request, std::vector<ipc::UniqueFd> file_descriptors,
                   ipc::IpcMessage *message) {
    if (message == nullptr || request.method == 0 ||
        file_descriptors.size() > ipc::kMaxFileDescriptors ||
        !valid_indices(request.fd_indices, file_descriptors.size())) {
        return -EINVAL;
    }
    const int length_status =
        validate_lengths(request.opaque.size(), request.fd_indices.size(), kRequestFixedSize);
    if (length_status != 0) {
        return length_status;
    }

    ipc::IpcMessage encoded;
    encoded.envelope.method = request.method;
    encoded.payload.reserve(kRequestFixedSize + request.opaque.size() +
                            request.fd_indices.size() * sizeof(std::uint32_t));
    append(&encoded.payload, htons(kControlPayloadVersion));
    append(&encoded.payload, std::uint16_t{0});
    append(&encoded.payload, host_to_network64(request.object_identity));
    append(&encoded.payload, host_to_network64(request.length));
    append(&encoded.payload, htonl(request.access));
    append(&encoded.payload, htonl(static_cast<std::uint32_t>(request.opaque.size())));
    append(&encoded.payload, htonl(static_cast<std::uint32_t>(request.fd_indices.size())));
    encoded.payload.insert(encoded.payload.end(), request.opaque.begin(), request.opaque.end());
    append_indices(&encoded.payload, request.fd_indices);
    encoded.file_descriptors = std::move(file_descriptors);
    encoded.envelope.payload_length = static_cast<std::uint32_t>(encoded.payload.size());
    encoded.envelope.fd_count = static_cast<std::uint32_t>(encoded.file_descriptors.size());
    *message = std::move(encoded);
    return 0;
}

int decode_request(ipc::IpcMessage message, DecodedControlRequest *request) {
    if (request == nullptr || message.envelope.method == 0 ||
        (message.envelope.flags & ipc::kResponseFlag) != 0) {
        return -EINVAL;
    }
    if (message.file_descriptors.size() > ipc::kMaxFileDescriptors ||
        message.envelope.fd_count != message.file_descriptors.size() ||
        message.envelope.payload_length != message.payload.size() ||
        message.payload.size() < kRequestFixedSize) {
        return -EPROTO;
    }
    std::size_t offset = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint64_t identity = 0;
    std::uint64_t length = 0;
    std::uint32_t access = 0;
    std::uint32_t opaque_size = 0;
    std::uint32_t index_count = 0;
    if (!read(message.payload, &offset, &version) || !read(message.payload, &offset, &reserved) ||
        !read(message.payload, &offset, &identity) || !read(message.payload, &offset, &length) ||
        !read(message.payload, &offset, &access) || !read(message.payload, &offset, &opaque_size) ||
        !read(message.payload, &offset, &index_count)) {
        return -EPROTO;
    }
    version = ntohs(version);
    if (version != kControlPayloadVersion || reserved != 0) {
        return -EPROTONOSUPPORT;
    }
    opaque_size = ntohl(opaque_size);
    index_count = ntohl(index_count);
    const std::size_t index_bytes = static_cast<std::size_t>(index_count) * sizeof(std::uint32_t);
    if (opaque_size > message.payload.size() - offset ||
        message.payload.size() - offset - opaque_size != index_bytes) {
        return -EPROTO;
    }

    DecodedControlRequest decoded;
    decoded.value.method = message.envelope.method;
    decoded.value.object_identity = network_to_host64(identity);
    decoded.value.length = network_to_host64(length);
    decoded.value.access = ntohl(access);
    decoded.value.opaque.assign(message.payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                message.payload.begin() +
                                    static_cast<std::ptrdiff_t>(offset + opaque_size));
    offset += opaque_size;
    if (!read_indices(message.payload, &offset, index_count, &decoded.value.fd_indices) ||
        !valid_indices(decoded.value.fd_indices, message.file_descriptors.size())) {
        return -EPROTO;
    }
    decoded.file_descriptors = std::move(message.file_descriptors);
    *request = std::move(decoded);
    return 0;
}

int encode_response(const UgdrControlResponse &response,
                    std::vector<ipc::UniqueFd> file_descriptors, ipc::IpcMessage *message) {
    if (message == nullptr || response.method == 0 ||
        file_descriptors.size() > ipc::kMaxFileDescriptors ||
        !valid_indices(response.fd_indices, file_descriptors.size())) {
        return -EINVAL;
    }
    const int length_status =
        validate_lengths(response.opaque.size(), response.fd_indices.size(), kResponseFixedSize);
    if (length_status != 0) {
        return length_status;
    }

    ipc::IpcMessage encoded;
    encoded.envelope.method = response.method;
    encoded.envelope.flags = ipc::kResponseFlag;
    encoded.envelope.status = response.status;
    encoded.payload.reserve(kResponseFixedSize + response.opaque.size() +
                            response.fd_indices.size() * sizeof(std::uint32_t));
    append(&encoded.payload, htons(kControlPayloadVersion));
    append(&encoded.payload, std::uint16_t{0});
    append(&encoded.payload, host_to_network64(response.object_identity));
    append(&encoded.payload, htonl(static_cast<std::uint32_t>(response.opaque.size())));
    append(&encoded.payload, htonl(static_cast<std::uint32_t>(response.fd_indices.size())));
    encoded.payload.insert(encoded.payload.end(), response.opaque.begin(), response.opaque.end());
    append_indices(&encoded.payload, response.fd_indices);
    encoded.file_descriptors = std::move(file_descriptors);
    encoded.envelope.payload_length = static_cast<std::uint32_t>(encoded.payload.size());
    encoded.envelope.fd_count = static_cast<std::uint32_t>(encoded.file_descriptors.size());
    *message = std::move(encoded);
    return 0;
}

int decode_response(ipc::IpcMessage message, DecodedControlResponse *response) {
    if (response == nullptr || message.envelope.method == 0 ||
        (message.envelope.flags & ipc::kResponseFlag) == 0) {
        return -EINVAL;
    }
    if (message.file_descriptors.size() > ipc::kMaxFileDescriptors ||
        message.envelope.fd_count != message.file_descriptors.size() ||
        message.envelope.payload_length != message.payload.size() ||
        message.payload.size() < kResponseFixedSize) {
        return -EPROTO;
    }
    std::size_t offset = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint64_t identity = 0;
    std::uint32_t opaque_size = 0;
    std::uint32_t index_count = 0;
    if (!read(message.payload, &offset, &version) || !read(message.payload, &offset, &reserved) ||
        !read(message.payload, &offset, &identity) ||
        !read(message.payload, &offset, &opaque_size) ||
        !read(message.payload, &offset, &index_count)) {
        return -EPROTO;
    }
    version = ntohs(version);
    if (version != kControlPayloadVersion || reserved != 0) {
        return -EPROTONOSUPPORT;
    }
    opaque_size = ntohl(opaque_size);
    index_count = ntohl(index_count);
    const std::size_t index_bytes = static_cast<std::size_t>(index_count) * sizeof(std::uint32_t);
    if (opaque_size > message.payload.size() - offset ||
        message.payload.size() - offset - opaque_size != index_bytes) {
        return -EPROTO;
    }

    DecodedControlResponse decoded;
    decoded.value.method = message.envelope.method;
    decoded.value.status = message.envelope.status;
    decoded.value.object_identity = network_to_host64(identity);
    decoded.value.opaque.assign(message.payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                message.payload.begin() +
                                    static_cast<std::ptrdiff_t>(offset + opaque_size));
    offset += opaque_size;
    if (!read_indices(message.payload, &offset, index_count, &decoded.value.fd_indices) ||
        !valid_indices(decoded.value.fd_indices, message.file_descriptors.size())) {
        return -EPROTO;
    }
    decoded.file_descriptors = std::move(message.file_descriptors);
    *response = std::move(decoded);
    return 0;
}

}  // namespace ugdr::control

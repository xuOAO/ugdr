#pragma once

#include "queue/shared_ring.hpp"

#include <arpa/inet.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ugdr::control {

constexpr std::uint16_t kQueueDescriptorPayloadVersion = 1;
constexpr std::size_t kQueueDescriptorWireSize = 16;

inline int encode_queue_descriptors(const std::vector<queue::QueueDescriptor> &descriptors,
                                    std::vector<std::byte> *bytes) {
    if (bytes == nullptr || descriptors.empty() || descriptors.size() > UINT16_MAX) {
        return EINVAL;
    }
    std::vector<std::byte> encoded;
    encoded.reserve(4 + descriptors.size() * kQueueDescriptorWireSize);
    const auto append = [&encoded](auto value) {
        const auto *begin = reinterpret_cast<const std::byte *>(&value);
        encoded.insert(encoded.end(), begin, begin + sizeof(value));
    };
    append(htons(kQueueDescriptorPayloadVersion));
    append(htons(static_cast<std::uint16_t>(descriptors.size())));
    for (const queue::QueueDescriptor &descriptor : descriptors) {
        append(htonl(static_cast<std::uint32_t>(descriptor.kind)));
        append(htonl(descriptor.capacity));
        append(htonl(descriptor.slot_stride));
        append(std::uint32_t{0});
    }
    *bytes = std::move(encoded);
    return 0;
}

inline int decode_queue_descriptors(const std::vector<std::byte> &bytes,
                                    std::vector<queue::QueueDescriptor> *descriptors) {
    if (descriptors == nullptr || bytes.size() < 4) {
        return EPROTO;
    }
    std::size_t offset = 0;
    const auto read = [&bytes, &offset](auto *value) {
        if (bytes.size() - offset < sizeof(*value)) {
            return false;
        }
        std::memcpy(value, bytes.data() + offset, sizeof(*value));
        offset += sizeof(*value);
        return true;
    };
    std::uint16_t version = 0;
    std::uint16_t count = 0;
    if (!read(&version) || !read(&count)) {
        return EPROTO;
    }
    if (ntohs(version) != kQueueDescriptorPayloadVersion) {
        return EPROTONOSUPPORT;
    }
    count = ntohs(count);
    if (count == 0 ||
        bytes.size() != 4 + static_cast<std::size_t>(count) * kQueueDescriptorWireSize) {
        return EPROTO;
    }
    std::vector<queue::QueueDescriptor> decoded;
    decoded.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        std::uint32_t kind = 0;
        std::uint32_t capacity = 0;
        std::uint32_t stride = 0;
        std::uint32_t reserved = 0;
        if (!read(&kind) || !read(&capacity) || !read(&stride) || !read(&reserved)) {
            return EPROTO;
        }
        kind = ntohl(kind);
        capacity = ntohl(capacity);
        stride = ntohl(stride);
        if (reserved != 0 || kind < static_cast<std::uint32_t>(queue::QueueKind::send) ||
            kind > static_cast<std::uint32_t>(queue::QueueKind::completion) || capacity == 0 ||
            stride == 0 || stride % queue::kSharedRingCacheLine != 0) {
            return EPROTO;
        }
        decoded.push_back({static_cast<queue::QueueKind>(kind), capacity, stride});
    }
    *descriptors = std::move(decoded);
    return 0;
}

}  // namespace ugdr::control

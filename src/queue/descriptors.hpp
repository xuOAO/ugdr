#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace ugdr::queue {

struct SharedSge {
    std::uint64_t address = 0;
    std::uint32_t length = 0;
    std::uint32_t lkey = 0;
};

struct SendWqeHeader {
    std::uint64_t wr_id = 0;
    std::uint64_t remote_address = 0;
    std::uint32_t rkey = 0;
    std::uint32_t opcode = 0;
    std::uint32_t send_flags = 0;
    std::uint32_t immediate_data = 0;
    std::uint32_t sge_count = 0;
    std::uint32_t reserved = 0;
};

struct ReceiveWqeHeader {
    std::uint64_t wr_id = 0;
    std::uint32_t sge_count = 0;
    std::uint32_t reserved = 0;
};

struct CompletionEntry {
    std::uint64_t wr_id = 0;
    std::uint32_t status = 0;
    std::uint32_t opcode = 0;
    std::uint32_t byte_length = 0;
    std::uint32_t immediate_data = 0;
    std::uint32_t qp_num = 0;
    std::uint32_t flags = 0;
};

static_assert(std::is_trivially_copyable_v<SharedSge>);
static_assert(std::is_trivially_copyable_v<SendWqeHeader>);
static_assert(std::is_trivially_copyable_v<ReceiveWqeHeader>);
static_assert(std::is_trivially_copyable_v<CompletionEntry>);

constexpr std::uint32_t kSlotAlignment = 64;

inline int aligned_slot_stride(std::size_t header_size, std::uint32_t max_sge,
                               std::uint32_t *slot_stride) noexcept {
    if (slot_stride == nullptr) {
        return EINVAL;
    }
    if (max_sge > (std::numeric_limits<std::size_t>::max() - header_size) / sizeof(SharedSge)) {
        return EOVERFLOW;
    }
    const std::size_t raw = header_size + static_cast<std::size_t>(max_sge) * sizeof(SharedSge);
    if (raw > std::numeric_limits<std::size_t>::max() - (kSlotAlignment - 1U)) {
        return EOVERFLOW;
    }
    const std::size_t aligned = (raw + kSlotAlignment - 1U) & ~(kSlotAlignment - 1U);
    if (aligned > std::numeric_limits<std::uint32_t>::max()) {
        return EOVERFLOW;
    }
    *slot_stride = static_cast<std::uint32_t>(aligned);
    return 0;
}

inline int send_slot_stride(std::uint32_t max_sge, std::uint32_t *slot_stride) noexcept {
    return aligned_slot_stride(sizeof(SendWqeHeader), max_sge, slot_stride);
}

inline int receive_slot_stride(std::uint32_t max_sge, std::uint32_t *slot_stride) noexcept {
    return aligned_slot_stride(sizeof(ReceiveWqeHeader), max_sge, slot_stride);
}

inline constexpr std::uint32_t completion_slot_stride() noexcept {
    return kSlotAlignment;
}

}  // namespace ugdr::queue

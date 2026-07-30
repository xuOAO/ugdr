#pragma once

#include <cstddef>
#include <cstdint>

namespace ugdr::gpu {

constexpr std::uint32_t kPersistentCudaCopyBackendCopyWarps = 30;
constexpr std::uint32_t kPersistentCudaCopyBackendDeviceBatch = 16;
constexpr std::uint32_t kPersistentCudaCopyBackendSharedQueueDepth = 16;
constexpr std::size_t kPersistentCudaCopyBackendHostBatch = 64;

struct PersistentCudaCopyBackendConfig {
    std::uint32_t device_ordinal = 0;
    std::uint64_t stage_buffer_base = 0;
    std::uint64_t stage_buffer_bytes = 0;
    std::size_t queue_capacity = 1024;
    std::size_t host_batch = kPersistentCudaCopyBackendHostBatch;
    std::uint64_t max_batch_delay_nanoseconds = 0;
};

int validate_persistent_cuda_copy_backend_config(
    const PersistentCudaCopyBackendConfig &config) noexcept;

}  // namespace ugdr::gpu

#include "gpu/persistent_copy_backend.hpp"

#include "gpu/persistent_copy.hpp"

#include <cerrno>
#include <limits>

namespace ugdr::gpu {
namespace {

bool is_power_of_two(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

}  // namespace

int validate_persistent_cuda_copy_backend_config(
    const PersistentCudaCopyBackendConfig &config) noexcept {
    if (config.stage_buffer_base == 0 || config.stage_buffer_bytes == 0 ||
        config.stage_buffer_bytes > kPersistentCopyMaxStageBufferBytes ||
        config.stage_buffer_base >
            std::numeric_limits<std::uint64_t>::max() - config.stage_buffer_bytes) {
        return EINVAL;
    }
    if (!is_power_of_two(config.queue_capacity) ||
        config.queue_capacity < kPersistentCudaCopyBackendHostBatch ||
        config.host_batch != kPersistentCudaCopyBackendHostBatch ||
        config.max_batch_delay_nanoseconds == 0) {
        return EINVAL;
    }
    return 0;
}

}  // namespace ugdr::gpu

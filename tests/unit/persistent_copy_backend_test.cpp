#include "gpu/persistent_copy.hpp"
#include "gpu/persistent_copy_backend.hpp"

#include <cerrno>
#include <cstdint>
#include <limits>

namespace {

ugdr::gpu::PersistentCudaCopyBackendConfig valid_config() {
    ugdr::gpu::PersistentCudaCopyBackendConfig config;
    config.stage_buffer_base = UINT64_C(0x100000);
    config.stage_buffer_bytes = 64 * 1024;
    config.queue_capacity = 1024;
    config.host_batch = ugdr::gpu::kPersistentCudaCopyBackendHostBatch;
    config.max_batch_delay_nanoseconds = 50'000;
    return config;
}

}  // namespace

int main() {
    using ugdr::gpu::validate_persistent_cuda_copy_backend_config;

    auto config = valid_config();
    if (validate_persistent_cuda_copy_backend_config(config) != 0) {
        return 1;
    }
    config.stage_buffer_base = 0;
    if (validate_persistent_cuda_copy_backend_config(config) != EINVAL) {
        return 2;
    }
    config = valid_config();
    config.stage_buffer_bytes = 0;
    if (validate_persistent_cuda_copy_backend_config(config) != EINVAL) {
        return 3;
    }
    config = valid_config();
    config.stage_buffer_bytes = ugdr::gpu::kPersistentCopyMaxStageBufferBytes + 1;
    if (validate_persistent_cuda_copy_backend_config(config) != EINVAL) {
        return 4;
    }
    config = valid_config();
    config.stage_buffer_base = std::numeric_limits<std::uint64_t>::max() - 15;
    config.stage_buffer_bytes = 16;
    if (validate_persistent_cuda_copy_backend_config(config) != EINVAL) {
        return 5;
    }
    config = valid_config();
    config.queue_capacity = 1000;
    if (validate_persistent_cuda_copy_backend_config(config) != EINVAL) {
        return 6;
    }
    config = valid_config();
    config.queue_capacity = ugdr::gpu::kPersistentCudaCopyBackendHostBatch / 2;
    if (validate_persistent_cuda_copy_backend_config(config) != EINVAL) {
        return 7;
    }
    config = valid_config();
    config.host_batch = ugdr::gpu::kPersistentCudaCopyBackendHostBatch / 2;
    if (validate_persistent_cuda_copy_backend_config(config) != EINVAL) {
        return 8;
    }
    config = valid_config();
    config.max_batch_delay_nanoseconds = 0;
    if (validate_persistent_cuda_copy_backend_config(config) != EINVAL) {
        return 9;
    }
    return 0;
}

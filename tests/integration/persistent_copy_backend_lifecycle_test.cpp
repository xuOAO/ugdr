#include "gpu/persistent_copy_backend.hpp"

#include <cuda_runtime_api.h>

#include <cstdint>

int main() {
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
        (void)cudaGetLastError();
        return 77;
    }

    void *stage_buffer = nullptr;
    constexpr std::size_t kStageBufferBytes = 64 * 1024;
    if (cudaSetDevice(0) != cudaSuccess ||
        cudaMalloc(&stage_buffer, kStageBufferBytes) != cudaSuccess) {
        (void)cudaGetLastError();
        return 77;
    }

    ugdr::gpu::PersistentCudaCopyBackendConfig config;
    config.device_ordinal = 0;
    config.stage_buffer_base =
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(stage_buffer));
    config.stage_buffer_bytes = kStageBufferBytes;
    config.queue_capacity = 64;
    config.max_batch_delay_nanoseconds = 50'000;

    ugdr::gpu::PersistentCudaCopyBackend backend;
    const int start_status = backend.start(config);
    const int stop_status = start_status == 0 ? backend.request_stop() : start_status;
    const int wait_status = stop_status == 0 ? backend.wait() : stop_status;
    const cudaError_t free_status = cudaFree(stage_buffer);
    if (start_status != 0 || stop_status != 0 || wait_status != 0 || free_status != cudaSuccess ||
        backend.state() != ugdr::gpu::PersistentCudaCopyBackendState::stopped) {
        return 1;
    }
    return 0;
}

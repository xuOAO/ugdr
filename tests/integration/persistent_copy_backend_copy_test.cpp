#include "gpu/persistent_copy.hpp"
#include "gpu/persistent_copy_backend.hpp"

#include <cuda_runtime_api.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>

namespace {

bool wait_for_completion(ugdr::gpu::PersistentCudaCopyBackend *backend,
                         ugdr::worker::BackendCompletion *completion) {
    for (int iteration = 0; iteration < 10000; ++iteration) {
        if (backend->try_pop_completion(*completion)) {
            return true;
        }
        ::usleep(100);
    }
    return false;
}

int run_copy(ugdr::gpu::PersistentCopyPayloadBuffer *payload,
             const ugdr::gpu::PersistentCudaCopyBackendConfig &config, std::uint64_t seed,
             std::size_t source_offset, std::size_t target_offset, std::size_t length,
             std::uint64_t parent_request_id) {
    if (payload->prepare(seed) != 0) {
        return 1;
    }
    ugdr::gpu::PersistentCudaCopyBackend backend;
    if (backend.start(config) != 0) {
        return 2;
    }

    ugdr::worker::BackendRequest request;
    request.parent_request_id = parent_request_id;
    request.parent_total_length = length;
    request.source_daemon_address = payload->stage_buffer_base() + source_offset;
    request.target_daemon_address = payload->target_address() + target_offset;
    request.payload_length = static_cast<std::uint32_t>(length);
    request.payload_count = 1;
    if (!backend.try_submit(request)) {
        return 3;
    }

    ugdr::worker::BackendCompletion completion;
    if (!wait_for_completion(&backend, &completion) ||
        completion.parent_request_id != parent_request_id || completion.payload_index != 0 ||
        completion.result != ugdr::worker::DatagramResult::success || backend.request_stop() != 0 ||
        backend.wait() != 0) {
        return 4;
    }
    ugdr::gpu::PayloadCheck check;
    if (payload->verify_copy(seed, source_offset, target_offset, length, &check) != 0 ||
        !check.payload_matches || !check.guards_intact || check.mismatch_count != 0) {
        return 5;
    }
    return 0;
}

}  // namespace

int main() {
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
        (void)cudaGetLastError();
        return 77;
    }

    constexpr std::size_t kPayloadCapacity = 16 * 1024;
    ugdr::gpu::PersistentCopyPayloadBuffer payload;
    if (ugdr::gpu::PersistentCopyPayloadBuffer::allocate(kPayloadCapacity, 64, &payload) != 0) {
        return 1;
    }
    ugdr::gpu::PersistentCudaCopyBackendConfig config;
    config.device_ordinal = 0;
    config.stage_buffer_base = payload.stage_buffer_base();
    config.stage_buffer_bytes = payload.payload_capacity();
    config.queue_capacity = 64;
    config.max_batch_delay_nanoseconds = 1;

    const int aligned_status = run_copy(&payload, config, UINT64_C(0x1234), 0, 0,
                                        ugdr::gpu::kPersistentCopyMaxPayloadBytes, 100);
    const int unaligned_status = aligned_status == 0
                                     ? run_copy(&payload, config, UINT64_C(0x5678), 3, 5,
                                                ugdr::gpu::kPersistentCopyMaxPayloadBytes - 1, 101)
                                     : 0;
    return aligned_status == 0 && unaligned_status == 0 ? 0 : 2;
}

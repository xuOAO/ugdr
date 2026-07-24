#include "gpu/persistent_copy.hpp"

#include <cuda.h>
#include <cuda_runtime_api.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace {

bool common_contract_smoke() {
    constexpr ugdr::gpu::PersistentCopyModel models[]{
        ugdr::gpu::PersistentCopyModel::direct_atomic,
        ugdr::gpu::PersistentCopyModel::dynamic_sharded_spsc,
        ugdr::gpu::PersistentCopyModel::static_partition_spsc,
        ugdr::gpu::PersistentCopyModel::warp_specialized,
    };
    for (const auto expected : models) {
        ugdr::gpu::PersistentCopyModel parsed{};
        const char *const name = ugdr::gpu::persistent_copy_model_name(expected);
        if (std::strcmp(name, "unknown") == 0 ||
            ugdr::gpu::parse_persistent_copy_model(name, &parsed) != 0 || parsed != expected) {
            return false;
        }
    }
    ugdr::gpu::PersistentCopyModel parsed{};
    if (ugdr::gpu::parse_persistent_copy_model("not_a_model", &parsed) != EINVAL ||
        ugdr::gpu::parse_persistent_copy_model(nullptr, &parsed) != EINVAL ||
        ugdr::gpu::parse_persistent_copy_model("direct_atomic", nullptr) != EINVAL) {
        return false;
    }

    ugdr::gpu::PersistentCopyConfig config;
    if (ugdr::gpu::validate_persistent_copy_config(config) != 0) {
        return false;
    }
    config.model = ugdr::gpu::PersistentCopyModel::warp_specialized;
    if (ugdr::gpu::validate_persistent_copy_config(config) != EINVAL) {
        return false;
    }
    config.shared_stage_count = 8;
    if (ugdr::gpu::validate_persistent_copy_config(config) != 0) {
        return false;
    }
    config.payload_bytes = ugdr::gpu::kPersistentCopyMaxPayloadBytes + 1;
    if (ugdr::gpu::validate_persistent_copy_config(config) != EINVAL) {
        return false;
    }
    config.payload_bytes = 8192;
    config.outstanding_capacity = 2;
    config.host_batch = 2;

    ugdr::gpu::PersistentCopyLifecycle lifecycle;
    if (lifecycle.start(config) != 0 ||
        lifecycle.state() != ugdr::gpu::PersistentCopyLifecycleState::accepting ||
        lifecycle.start(config) != EBUSY || lifecycle.record_accepted(2) != 0 ||
        lifecycle.record_accepted() != EAGAIN || lifecycle.record_completed(1) != 0 ||
        lifecycle.record_accepted() != 0 || lifecycle.request_stop() != 0 ||
        lifecycle.record_accepted() != EINVAL || lifecycle.finish_stop() != EAGAIN ||
        lifecycle.record_completed(2) != 0 || !lifecycle.drained() ||
        lifecycle.finish_stop() != 0 ||
        lifecycle.state() != ugdr::gpu::PersistentCopyLifecycleState::stopped ||
        lifecycle.accepted_tasks() != 3 || lifecycle.completed_tasks() != 3 ||
        lifecycle.start(config) != 0 || lifecycle.request_stop() != 0 ||
        lifecycle.finish_stop() != 0) {
        return false;
    }

    return sizeof(ugdr::gpu::CopyTask) % 16 == 0 && sizeof(ugdr::gpu::CopyCompletion) % 16 == 0 &&
           ugdr::gpu::persistent_copy_payload_byte(7, 3) ==
               ugdr::gpu::persistent_copy_payload_byte(7, 3) &&
           ugdr::gpu::persistent_copy_payload_byte(7, 3) !=
               ugdr::gpu::persistent_copy_payload_byte(7, 4);
}

int gpu_resource_smoke() {
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
        (void)cudaGetLastError();
        return 77;
    }
    if (ugdr::gpu::initialize_persistent_copy_device(0) != 0 ||
        ugdr::gpu::initialize_persistent_copy_device(static_cast<std::uint32_t>(device_count)) !=
            EINVAL) {
        return 9;
    }

    ugdr::gpu::MappedPinnedMemory mapped;
    if (ugdr::gpu::MappedPinnedMemory::allocate(4096, &mapped) != 0 || mapped.empty() ||
        mapped.host_data() == nullptr || mapped.device_address() == 0 || mapped.size() != 4096 ||
        ugdr::gpu::MappedPinnedMemory::allocate(64, &mapped) != EINVAL) {
        return 10;
    }
    auto *const host_bytes = static_cast<std::uint8_t *>(mapped.host_data());
    for (std::size_t index = 0; index < mapped.size(); ++index) {
        host_bytes[index] = static_cast<std::uint8_t>(index);
    }
    cudaPointerAttributes attributes{};
    if (cudaPointerGetAttributes(&attributes, mapped.host_data()) != cudaSuccess ||
        attributes.type != cudaMemoryTypeHost || attributes.devicePointer == nullptr) {
        return 11;
    }
    ugdr::gpu::MappedPinnedMemory moved = std::move(mapped);
    if (!mapped.empty() || moved.empty() || moved.reset() != 0 || !moved.empty() ||
        moved.reset() != 0) {
        return 12;
    }
    for (int iteration = 0; iteration < 32; ++iteration) {
        ugdr::gpu::MappedPinnedMemory allocation;
        if (ugdr::gpu::MappedPinnedMemory::allocate(256, &allocation) != 0 ||
            allocation.reset() != 0) {
            return 13;
        }
    }

    ugdr::gpu::PersistentCopyPayloadBuffer payload;
    constexpr std::size_t payload_bytes = 8192;
    constexpr std::size_t guard_bytes = 16;
    constexpr std::uint64_t seed = UINT64_C(0x123456789abcdef0);
    if (ugdr::gpu::PersistentCopyPayloadBuffer::allocate(payload_bytes, guard_bytes, &payload) !=
            0 ||
        payload.prepare(seed) != 0 || payload.payload_capacity() != payload_bytes ||
        payload.guard_bytes() != guard_bytes || payload.source_address() == 0 ||
        payload.target_address() == 0) {
        return 14;
    }
    ugdr::gpu::CopyTask task;
    if (payload.make_task(17, 23, 2, payload_bytes, &task) != 0 || task.task_id != 17 ||
        task.parent_request_id != 23 || task.payload_index != 2 ||
        task.source_address != payload.source_address() ||
        task.target_address != payload.target_address() || task.length != payload_bytes ||
        payload.make_task(1, 1, 0, payload_bytes + 1, &task) != EINVAL) {
        return 15;
    }
    if (cuMemcpyDtoD(static_cast<CUdeviceptr>(task.target_address),
                     static_cast<CUdeviceptr>(task.source_address), task.length) != CUDA_SUCCESS ||
        cuCtxSynchronize() != CUDA_SUCCESS) {
        return 16;
    }
    ugdr::gpu::PayloadCheck check;
    if (payload.verify(seed, &check) != 0 || !check.payload_matches || !check.guards_intact ||
        check.mismatch_count != 0 || check.first_mismatch != payload_bytes) {
        return 17;
    }
    if (cuMemsetD8(static_cast<CUdeviceptr>(task.target_address - 1), 0, 1) != CUDA_SUCCESS ||
        cuCtxSynchronize() != CUDA_SUCCESS || payload.verify(seed, &check) != 0 ||
        !check.payload_matches || check.guards_intact) {
        return 18;
    }
    ugdr::gpu::PersistentCopyPayloadBuffer moved_payload = std::move(payload);
    if (!payload.empty() || moved_payload.empty() || moved_payload.reset() != 0 ||
        !moved_payload.empty() || moved_payload.reset() != 0) {
        return 19;
    }
    return 0;
}

}  // namespace

int main() {
    if (!common_contract_smoke()) {
        return 1;
    }
    return gpu_resource_smoke();
}

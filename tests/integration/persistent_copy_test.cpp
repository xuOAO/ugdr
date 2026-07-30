#include "gpu/persistent_copy.hpp"

#include <cuda.h>
#include <cuda_runtime_api.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace {

bool kernel_resources_match(const ugdr::gpu::KernelResourceUsage &resources,
                            std::size_t shared_memory_bytes) {
    return resources.shared_memory_bytes == shared_memory_bytes &&
           resources.registers_per_thread != 0 && resources.occupancy > 0.0 &&
           resources.occupancy <= 1.0;
}

bool common_contract_smoke() {
    constexpr ugdr::gpu::PersistentCopyModel models[]{
        ugdr::gpu::PersistentCopyModel::direct_atomic,
        ugdr::gpu::PersistentCopyModel::dynamic_sharded_spsc,
        ugdr::gpu::PersistentCopyModel::static_partition_spsc,
        ugdr::gpu::PersistentCopyModel::warp_specialized,
        ugdr::gpu::PersistentCopyModel::warp_specialized_pipeline,
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
    config.shared_queue_depth = 8;
    if (ugdr::gpu::validate_persistent_copy_config(config) != 0) {
        return false;
    }
    config.shared_queue_depth = 3;
    if (ugdr::gpu::validate_persistent_copy_config(config) != EINVAL) {
        return false;
    }
    config.shared_queue_depth = 2;
    if (ugdr::gpu::validate_persistent_copy_config(config) != EINVAL) {
        return false;
    }
    config.shared_queue_depth = 8;
    config.payload_bytes = ugdr::gpu::kPersistentCopyMaxPayloadBytes + 1;
    if (ugdr::gpu::validate_persistent_copy_config(config) != EINVAL) {
        return false;
    }
    config.payload_bytes = 8192;
    config.outstanding_capacity = 3;
    config.host_batch = 2;
    if (ugdr::gpu::validate_persistent_copy_config(config) != EINVAL) {
        return false;
    }
    config.outstanding_capacity = 2;
    config.host_batch = 2;
    config.device_batch = 4;
    if (ugdr::gpu::validate_persistent_copy_config(config) != EINVAL) {
        return false;
    }
    config.device_batch = 2;
    config.model = ugdr::gpu::PersistentCopyModel::direct_atomic;
    config.copy_warps = 32;
    if (ugdr::gpu::validate_persistent_copy_config(config) != 0) {
        return false;
    }
    config.copy_warps = 33;
    if (ugdr::gpu::validate_persistent_copy_config(config) != EINVAL) {
        return false;
    }
    config.copy_warps = 2;
    if (ugdr::gpu::validate_persistent_copy_config(config) != EINVAL) {
        return false;
    }
    config.model = ugdr::gpu::PersistentCopyModel::warp_specialized;
    config.copy_warps = 32;
    if (ugdr::gpu::validate_persistent_copy_config(config) != EINVAL) {
        return false;
    }
    config.model = ugdr::gpu::PersistentCopyModel::direct_atomic;
    config.copy_warps = 4;
    config.device_batch = 3;
    if (ugdr::gpu::validate_persistent_copy_config(config) != EINVAL) {
        return false;
    }
    config.device_batch = 2;
    config.model = ugdr::gpu::PersistentCopyModel::dynamic_sharded_spsc;
    config.outstanding_capacity = 32;
    config.copy_warps = 3;
    if (ugdr::gpu::validate_persistent_copy_config(config) != EINVAL) {
        return false;
    }
    config.model = ugdr::gpu::PersistentCopyModel::direct_atomic;
    config.outstanding_capacity = 2;
    config.copy_warps = 4;
    config.model = ugdr::gpu::PersistentCopyModel::static_partition_spsc;
    config.outstanding_capacity = 32;
    config.copy_warps = 3;
    if (ugdr::gpu::validate_persistent_copy_config(config) != EINVAL) {
        return false;
    }
    config.model = ugdr::gpu::PersistentCopyModel::direct_atomic;
    config.outstanding_capacity = 2;
    config.copy_warps = 4;
    config.model = ugdr::gpu::PersistentCopyModel::warp_specialized_pipeline;
    config.outstanding_capacity = 32;
    config.device_batch = 16;
    config.shared_queue_depth = 8;
    if (ugdr::gpu::validate_persistent_copy_config(config) != EINVAL) {
        return false;
    }
    config.device_batch = ugdr::gpu::kWarpSpecializedPipelineMetaBatch;
    config.copy_warps = 2;
    if (ugdr::gpu::validate_persistent_copy_config(config) != EINVAL) {
        return false;
    }
    config.shared_queue_depth = 16;
    if (ugdr::gpu::validate_persistent_copy_config(config) != 0) {
        return false;
    }
    config.model = ugdr::gpu::PersistentCopyModel::direct_atomic;
    config.outstanding_capacity = 2;
    config.device_batch = 2;
    config.copy_warps = 4;

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

    ugdr::gpu::CopyCompletion completion;
    completion.task_id = 17;
    completion.result = ugdr::gpu::CopyTaskResult::copy_failed;
    ugdr::gpu::PersistentCopyPayloadBuffer oversized_stage_buffer;
    if constexpr (std::numeric_limits<std::size_t>::max() >
                  ugdr::gpu::kPersistentCopyMaxStageBufferBytes) {
        if (ugdr::gpu::PersistentCopyPayloadBuffer::allocate(
                static_cast<std::size_t>(ugdr::gpu::kPersistentCopyMaxStageBufferBytes + 1), 16,
                &oversized_stage_buffer) != EINVAL) {
            return false;
        }
    }
    return sizeof(ugdr::gpu::CopyTask) == 32 && sizeof(ugdr::gpu::CopyCompletion) == 16 &&
           completion.task_id == 17 &&
           completion.result == ugdr::gpu::CopyTaskResult::copy_failed &&
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
    if (!mapped.empty() ||  // NOLINT(bugprone-use-after-move): verifies the moved-from contract.
        moved.empty() || moved.reset() != 0 || !moved.empty() || moved.reset() != 0) {
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
        payload.guard_bytes() != guard_bytes || payload.stage_buffer_base() == 0 ||
        payload.target_address() == 0) {
        return 14;
    }
    ugdr::gpu::CopyTask task;
    if (payload.make_task(17, payload.target_address(), payload_bytes, 0, &task) != 0 ||
        task.task_id != 17 || task.target_address != payload.target_address() ||
        task.length != payload_bytes || task.relative_offset != 0 ||
        payload.make_task(1, payload.target_address(), payload_bytes + 1, 0, &task) != EINVAL ||
        payload.make_task(1, payload.target_address(), 2,
                          static_cast<std::uint32_t>(payload_bytes - 1), &task) != EINVAL ||
        payload.make_task(1, 0, payload_bytes, 0, &task) != EINVAL) {
        return 15;
    }
    ugdr::gpu::CopyTask offset_task;
    constexpr std::uint32_t relative_offset = 16;
    if (payload.make_task(18, payload.target_address() + relative_offset,
                          payload_bytes - relative_offset, relative_offset, &offset_task) != 0 ||
        offset_task.task_id != 18 ||
        offset_task.target_address != payload.target_address() + relative_offset ||
        offset_task.length != payload_bytes - relative_offset ||
        offset_task.relative_offset != relative_offset) {
        return 15;
    }
    const std::uint64_t source_address = payload.stage_buffer_base() + task.relative_offset;
    if (cuMemcpyDtoD(static_cast<CUdeviceptr>(task.target_address),
                     static_cast<CUdeviceptr>(source_address), task.length) != CUDA_SUCCESS ||
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
    if (!payload.empty() ||  // NOLINT(bugprone-use-after-move): verifies moved-from ownership.
        moved_payload.empty() || moved_payload.reset() != 0 || !moved_payload.empty() ||
        moved_payload.reset() != 0) {
        return 19;
    }
    return 0;
}

int copy_core_matrix_smoke() {
    constexpr std::size_t max_payload_bytes = ugdr::gpu::kPersistentCopyMaxPayloadBytes;
    constexpr std::size_t max_offset = 15;
    constexpr std::size_t payload_capacity = max_payload_bytes + max_offset;
    constexpr std::size_t guard_bytes = 16;
    constexpr std::uint64_t seed = UINT64_C(0xc031c0dec031c0de);
    constexpr std::size_t lengths[]{1, 15, 16, 17, 8191, 8192};

    ugdr::gpu::PersistentCopyPayloadBuffer payload;
    if (ugdr::gpu::PersistentCopyPayloadBuffer::allocate(payload_capacity, guard_bytes, &payload) !=
        0) {
        return 20;
    }
    if ((payload.stage_buffer_base() & 15U) != 0 || (payload.target_address() & 15U) != 0) {
        return 21;
    }

    ugdr::gpu::MappedPinnedMemory access_counts_memory;
    if (ugdr::gpu::MappedPinnedMemory::allocate(sizeof(ugdr::gpu::CopyAccessCounts),
                                                &access_counts_memory) != 0) {
        return 22;
    }
    auto *const access_counts =
        static_cast<ugdr::gpu::CopyAccessCounts *>(access_counts_memory.host_data());

    std::uint64_t task_id = 1;
    std::size_t case_count = 0;
    for (std::uint32_t source_offset = 0; source_offset <= max_offset; ++source_offset) {
        for (std::uint32_t target_offset = 0; target_offset <= max_offset; ++target_offset) {
            for (const std::size_t length : lengths) {
                if (payload.prepare(seed) != 0) {
                    return 23;
                }
                ugdr::gpu::CopyTask task;
                if (payload.make_task(task_id++, payload.target_address() + target_offset, length,
                                      source_offset, &task) != 0) {
                    return 24;
                }
                std::memset(access_counts, 0xa5, sizeof(*access_counts));
                if (ugdr::gpu::launch_persistent_copy_core_test(
                        payload.stage_buffer_base(), task, access_counts_memory.device_address()) !=
                        0 ||
                    cuCtxSynchronize() != CUDA_SUCCESS) {
                    return 25;
                }

                const std::uint64_t source_address = payload.stage_buffer_base() + source_offset;
                const std::uint64_t target_address = payload.target_address() + target_offset;
                std::size_t expected_vector_bytes = 0;
                if ((source_address & 15U) == (target_address & 15U)) {
                    std::size_t prefix = (16 - (source_address & 15U)) & 15U;
                    if (prefix > length) {
                        prefix = length;
                    }
                    expected_vector_bytes = ((length - prefix) / 16) * 16;
                }
                if (access_counts->copied_bytes != length ||
                    access_counts->vector_128_bytes != expected_vector_bytes ||
                    access_counts->narrow_bytes != length - expected_vector_bytes) {
                    return 26;
                }

                ugdr::gpu::PayloadCheck check;
                if (payload.verify_copy(seed, source_offset, target_offset, length, &check) != 0 ||
                    !check.payload_matches || !check.guards_intact || check.mismatch_count != 0 ||
                    check.first_mismatch != payload_capacity) {
                    return 27;
                }
                ++case_count;
            }
        }
    }
    if (case_count != 16 * 16 * 6) {
        return 28;
    }

    ugdr::gpu::CopyTask valid_task;
    if (payload.make_task(task_id, payload.target_address(), 1, 0, &valid_task) != 0 ||
        ugdr::gpu::launch_persistent_copy_core_test(
            0, valid_task, access_counts_memory.device_address()) != EINVAL ||
        ugdr::gpu::launch_persistent_copy_core_test(payload.stage_buffer_base(), valid_task, 0) !=
            EINVAL) {
        return 29;
    }
    return 0;
}

int direct_atomic_queue_smoke(std::uint32_t device_batch) {
    constexpr std::size_t payload_bytes = 8192;
    constexpr std::size_t guard_bytes = 16;
    constexpr std::size_t capacity = 32;
    constexpr std::uint64_t seed = UINT64_C(0xda7a70c0da7a70c0);
    constexpr std::size_t total_tasks = 257;

    ugdr::gpu::PersistentCopyPayloadBuffer payload;
    if (ugdr::gpu::PersistentCopyPayloadBuffer::allocate(payload_bytes, guard_bytes, &payload) !=
            0 ||
        payload.prepare(seed) != 0) {
        return 30;
    }
    ugdr::gpu::DirectAtomicQueue queue;
    if (ugdr::gpu::DirectAtomicQueue::allocate(3, 4, 4, payload.stage_buffer_base(), &queue) !=
            EINVAL ||
        ugdr::gpu::DirectAtomicQueue::allocate(capacity, 2, 4, payload.stage_buffer_base(),
                                               &queue) != EINVAL ||
        ugdr::gpu::DirectAtomicQueue::allocate(8, 4, 16, payload.stage_buffer_base(), &queue) !=
            EINVAL ||
        ugdr::gpu::DirectAtomicQueue::allocate(capacity, 4, 3, payload.stage_buffer_base(),
                                               &queue) != EINVAL ||
        ugdr::gpu::DirectAtomicQueue::allocate(capacity, 4, device_batch,
                                               payload.stage_buffer_base(), &queue) != 0 ||
        queue.capacity() != capacity || queue.copy_warps() != 4 ||
        queue.device_batch() != device_batch || queue.host_meta_bytes() != 64 + capacity * 64 ||
        queue.start() != 0 || !queue.running() || !queue.accepting() ||
        !kernel_resources_match(queue.kernel_resources(), 0)) {
        return 31;
    }
    ugdr::gpu::CopyCompletion completion;
    std::size_t submitted_count = 99;
    ugdr::gpu::CopyTask invalid_batch[2]{};
    if (payload.make_task(1, payload.target_address(), payload_bytes, 0, &invalid_batch[0]) != 0) {
        return 32;
    }
    if (queue.try_poll(&completion) != EAGAIN ||
        queue.try_submit_batch(nullptr, 1, &submitted_count) != EINVAL || submitted_count != 0 ||
        queue.try_submit_batch(nullptr, 0, nullptr) != EINVAL ||
        queue.try_submit_batch(invalid_batch, 2, &submitted_count) != EINVAL ||
        submitted_count != 0 || queue.accepted_tasks() != 0) {
        return 32;
    }

    std::vector<bool> seen(total_tasks, false);
    std::vector<ugdr::gpu::CopyTask> submit_batch(5);
    std::size_t submitted = 0;
    std::size_t completed = 0;
    std::size_t stalled = 0;
    while (completed != total_tasks) {
        bool progressed = false;
        while (submitted != total_tasks) {
            const std::size_t remaining = total_tasks - submitted;
            const std::size_t requested =
                remaining < submit_batch.size() ? remaining : submit_batch.size();
            for (std::size_t index = 0; index < requested; ++index) {
                if (payload.make_task(submitted + index + 1, payload.target_address(),
                                      payload_bytes, 0, &submit_batch[index]) != 0) {
                    return 33;
                }
            }
            submitted_count = 0;
            const int status =
                queue.try_submit_batch(submit_batch.data(), requested, &submitted_count);
            if (status == EAGAIN) {
                break;
            }
            if (status != 0 || submitted_count == 0 || submitted_count > requested) {
                return 34;
            }
            submitted += submitted_count;
            progressed = true;
            if (submitted_count != requested) {
                break;
            }
        }
        while (queue.try_poll(&completion) == 0) {
            if (completion.task_id == 0 || completion.task_id > total_tasks ||
                completion.result != ugdr::gpu::CopyTaskResult::success ||
                seen[completion.task_id - 1]) {
                return 35;
            }
            seen[completion.task_id - 1] = true;
            ++completed;
            progressed = true;
        }
        stalled = progressed ? 0 : stalled + 1;
        if (stalled > 10000000) {
            return 36;
        }
    }
    ugdr::gpu::CopyTask overflow_task;
    if (payload.make_task(total_tasks + 1, payload.target_address(), payload_bytes, 0,
                          &overflow_task) != 0 ||
        !queue.drained() || queue.accepted_tasks() != total_tasks ||
        queue.completed_tasks() != total_tasks || queue.host_system_atomic_operations() == 0 ||
        queue.host_system_atomic_operations() > total_tasks * 2 ||
        (queue.host_system_atomic_operations() & 1U) != 0 || queue.request_stop() != 0 ||
        queue.try_submit(overflow_task) != EINVAL || queue.wait() != 0 || queue.running() ||
        queue.wait() != EINVAL) {
        return 37;
    }

    if (queue.start() != 0) {
        return 38;
    }
    for (std::size_t index = 0; index < capacity; ++index) {
        ugdr::gpu::CopyTask task;
        if (payload.make_task(1000 + index, payload.target_address(), payload_bytes, 0, &task) !=
                0 ||
            queue.try_submit(task) != 0) {
            return 39;
        }
    }
    if (queue.try_submit(overflow_task) != EAGAIN || queue.request_stop() != 0 ||
        queue.wait() != 0 || queue.drained()) {
        return 40;
    }
    std::vector<bool> drained_seen(capacity, false);
    for (std::size_t index = 0; index < capacity; ++index) {
        if (queue.try_poll(&completion) != 0 || completion.task_id < 1000 ||
            completion.task_id >= 1000 + capacity || drained_seen[completion.task_id - 1000]) {
            return 41;
        }
        drained_seen[completion.task_id - 1000] = true;
    }
    ugdr::gpu::PayloadCheck check;
    if (!queue.drained() || queue.try_poll(&completion) != EAGAIN ||
        payload.verify(seed, &check) != 0 || !check.payload_matches || !check.guards_intact ||
        queue.reset() != 0 || !queue.empty()) {
        return 42;
    }
    return 0;
}

int dynamic_sharded_spsc_queue_smoke(std::uint32_t copy_warps, std::uint32_t device_batch) {
    constexpr std::size_t payload_bytes = 8192;
    constexpr std::size_t guard_bytes = 16;
    constexpr std::size_t capacity = 32;
    constexpr std::uint64_t seed = UINT64_C(0xd1a6c5c5d1a6c5c5);
    constexpr std::size_t total_tasks = 257;

    ugdr::gpu::PersistentCopyPayloadBuffer payload;
    if (ugdr::gpu::PersistentCopyPayloadBuffer::allocate(payload_bytes, guard_bytes, &payload) !=
            0 ||
        payload.prepare(seed) != 0) {
        return 43;
    }
    ugdr::gpu::DynamicShardedSpscQueue queue;
    if (ugdr::gpu::DynamicShardedSpscQueue::allocate(3, 4, 4, payload.stage_buffer_base(),
                                                     &queue) != EINVAL ||
        ugdr::gpu::DynamicShardedSpscQueue::allocate(8, 16, 4, payload.stage_buffer_base(),
                                                     &queue) != EINVAL ||
        ugdr::gpu::DynamicShardedSpscQueue::allocate(capacity, 3, 4, payload.stage_buffer_base(),
                                                     &queue) != EINVAL ||
        ugdr::gpu::DynamicShardedSpscQueue::allocate(
            capacity, copy_warps, 3, payload.stage_buffer_base(), &queue) != EINVAL ||
        ugdr::gpu::DynamicShardedSpscQueue::allocate(capacity, copy_warps, device_batch,
                                                     payload.stage_buffer_base(), &queue) != 0 ||
        queue.capacity() != capacity || queue.lane_capacity() != capacity / copy_warps ||
        queue.lane_count() != copy_warps || queue.device_batch() != device_batch ||
        queue.host_meta_bytes() != copy_warps * 64 + capacity * 48 ||
        queue.host_system_atomic_operations() != 0 || queue.start() != 0 || !queue.running() ||
        !queue.accepting() || !kernel_resources_match(queue.kernel_resources(), 0)) {
        return 44;
    }

    ugdr::gpu::CopyCompletion completion;
    std::size_t submitted_count = 99;
    ugdr::gpu::CopyTask invalid_batch[2]{};
    if (payload.make_task(1, payload.target_address(), payload_bytes, 0, &invalid_batch[0]) != 0) {
        return 45;
    }
    if (queue.try_poll(&completion) != EAGAIN ||
        queue.try_submit_batch(nullptr, 1, &submitted_count) != EINVAL || submitted_count != 0 ||
        queue.try_submit_batch(nullptr, 0, nullptr) != EINVAL ||
        queue.try_submit_batch(invalid_batch, 2, &submitted_count) != EINVAL ||
        submitted_count != 0 || queue.accepted_tasks() != 0) {
        return 45;
    }

    std::vector<bool> seen(total_tasks, false);
    std::vector<ugdr::gpu::CopyTask> submit_batch(7);
    std::size_t submitted = 0;
    std::size_t completed = 0;
    std::size_t stalled = 0;
    while (completed != total_tasks) {
        bool progressed = false;
        while (submitted != total_tasks) {
            const std::size_t remaining = total_tasks - submitted;
            const std::size_t requested =
                remaining < submit_batch.size() ? remaining : submit_batch.size();
            for (std::size_t index = 0; index < requested; ++index) {
                if (payload.make_task(submitted + index + 1, payload.target_address(),
                                      payload_bytes, 0, &submit_batch[index]) != 0) {
                    return 46;
                }
            }
            submitted_count = 0;
            const int status =
                queue.try_submit_batch(submit_batch.data(), requested, &submitted_count);
            if (status == EAGAIN) {
                break;
            }
            if (status != 0 || submitted_count == 0 || submitted_count > requested) {
                return 47;
            }
            submitted += submitted_count;
            progressed = true;
            if (submitted_count != requested) {
                break;
            }
        }
        while (queue.try_poll(&completion) == 0) {
            if (completion.task_id == 0 || completion.task_id > total_tasks ||
                completion.result != ugdr::gpu::CopyTaskResult::success ||
                seen[completion.task_id - 1]) {
                return 48;
            }
            seen[completion.task_id - 1] = true;
            ++completed;
            progressed = true;
        }
        stalled = progressed ? 0 : stalled + 1;
        if (stalled > 10000000) {
            return 49;
        }
    }

    ugdr::gpu::CopyTask overflow_task;
    if (payload.make_task(total_tasks + 1, payload.target_address(), payload_bytes, 0,
                          &overflow_task) != 0 ||
        !queue.drained() || queue.accepted_tasks() != total_tasks ||
        queue.completed_tasks() != total_tasks || queue.host_system_atomic_operations() != 0 ||
        queue.request_stop() != 0 || queue.try_submit(overflow_task) != EINVAL ||
        queue.wait() != 0 || queue.running() || queue.wait() != EINVAL) {
        return 50;
    }

    if (queue.start() != 0) {
        return 51;
    }
    for (std::size_t index = 0; index < capacity; ++index) {
        ugdr::gpu::CopyTask task;
        if (payload.make_task(2000 + index, payload.target_address(), payload_bytes, 0, &task) !=
                0 ||
            queue.try_submit(task) != 0) {
            return 52;
        }
    }
    if (queue.try_submit(overflow_task) != EAGAIN || queue.request_stop() != 0 ||
        queue.wait() != 0 || queue.drained()) {
        return 53;
    }
    std::vector<bool> drained_seen(capacity, false);
    for (std::size_t index = 0; index < capacity; ++index) {
        if (queue.try_poll(&completion) != 0 || completion.task_id < 2000 ||
            completion.task_id >= 2000 + capacity || drained_seen[completion.task_id - 2000]) {
            return 54;
        }
        drained_seen[completion.task_id - 2000] = true;
    }
    ugdr::gpu::PayloadCheck check;
    if (!queue.drained() || queue.try_poll(&completion) != EAGAIN ||
        payload.verify(seed, &check) != 0 || !check.payload_matches || !check.guards_intact ||
        queue.reset() != 0 || !queue.empty()) {
        return 55;
    }
    return 0;
}

int static_partition_spsc_queue_smoke(std::uint32_t copy_warps, std::uint32_t device_batch) {
    constexpr std::size_t payload_bytes = 8192;
    constexpr std::size_t guard_bytes = 16;
    constexpr std::size_t capacity = 32;
    constexpr std::uint64_t seed = UINT64_C(0x57a71c5a57a71c5a);
    constexpr std::size_t total_tasks = 257;

    ugdr::gpu::PersistentCopyPayloadBuffer payload;
    if (ugdr::gpu::PersistentCopyPayloadBuffer::allocate(payload_bytes, guard_bytes, &payload) !=
            0 ||
        payload.prepare(seed) != 0) {
        return 56;
    }
    ugdr::gpu::StaticPartitionSpscQueue queue;
    if (ugdr::gpu::StaticPartitionSpscQueue::allocate(3, 4, 4, payload.stage_buffer_base(),
                                                      &queue) != EINVAL ||
        ugdr::gpu::StaticPartitionSpscQueue::allocate(8, 16, 4, payload.stage_buffer_base(),
                                                      &queue) != EINVAL ||
        ugdr::gpu::StaticPartitionSpscQueue::allocate(capacity, 3, 4, payload.stage_buffer_base(),
                                                      &queue) != EINVAL ||
        ugdr::gpu::StaticPartitionSpscQueue::allocate(
            capacity, copy_warps, 3, payload.stage_buffer_base(), &queue) != EINVAL ||
        ugdr::gpu::StaticPartitionSpscQueue::allocate(capacity, copy_warps, device_batch,
                                                      payload.stage_buffer_base(), &queue) != 0 ||
        queue.capacity() != capacity || queue.partition_capacity() != capacity / copy_warps ||
        queue.copy_warps() != copy_warps || queue.device_batch() != device_batch ||
        queue.host_meta_bytes() != 64 + capacity * 64 ||
        queue.host_system_atomic_operations() != 0 || queue.start() != 0 || !queue.running() ||
        !queue.accepting() || !kernel_resources_match(queue.kernel_resources(), 0)) {
        return 57;
    }

    ugdr::gpu::CopyCompletion completion;
    std::size_t submitted_count = 99;
    ugdr::gpu::CopyTask invalid_batch[2]{};
    if (payload.make_task(1, payload.target_address(), payload_bytes, 0, &invalid_batch[0]) != 0 ||
        queue.try_poll(&completion) != EAGAIN ||
        queue.try_submit_batch(nullptr, 1, &submitted_count) != EINVAL || submitted_count != 0 ||
        queue.try_submit_batch(nullptr, 0, nullptr) != EINVAL ||
        queue.try_submit_batch(invalid_batch, 2, &submitted_count) != EINVAL ||
        submitted_count != 0 || queue.accepted_tasks() != 0) {
        return 58;
    }

    std::vector<ugdr::gpu::CopyTask> submit_batch(7);
    std::size_t submitted = 0;
    std::size_t completed = 0;
    std::size_t stalled = 0;
    while (completed != total_tasks) {
        bool progressed = false;
        while (submitted != total_tasks) {
            const std::size_t remaining = total_tasks - submitted;
            const std::size_t requested =
                remaining < submit_batch.size() ? remaining : submit_batch.size();
            for (std::size_t index = 0; index < requested; ++index) {
                if (payload.make_task(submitted + index + 1, payload.target_address(),
                                      payload_bytes, 0, &submit_batch[index]) != 0) {
                    return 59;
                }
            }
            submitted_count = 0;
            const int status =
                queue.try_submit_batch(submit_batch.data(), requested, &submitted_count);
            if (status == EAGAIN) {
                break;
            }
            if (status != 0 || submitted_count == 0 || submitted_count > requested) {
                return 60;
            }
            submitted += submitted_count;
            progressed = true;
            if (submitted_count != requested) {
                break;
            }
        }
        while (queue.try_poll(&completion) == 0) {
            if (completion.task_id != completed + 1 ||
                completion.result != ugdr::gpu::CopyTaskResult::success) {
                return 61;
            }
            ++completed;
            progressed = true;
        }
        stalled = progressed ? 0 : stalled + 1;
        if (stalled > 10000000) {
            return 62;
        }
    }

    ugdr::gpu::PayloadCheck check;
    if (!queue.drained() || queue.accepted_tasks() != total_tasks ||
        queue.completed_tasks() != total_tasks || queue.host_system_atomic_operations() != 0) {
        return 63;
    }
    if (queue.request_stop() != 0 || queue.wait() != 0 || queue.running() ||
        queue.wait() != EINVAL) {
        return 63;
    }
    if (payload.verify(seed, &check) != 0 || !check.payload_matches || !check.guards_intact) {
        return 63;
    }

    if (copy_warps == 4 && device_batch == 4) {
        if (queue.start() != 0) {
            return 64;
        }
        for (std::size_t index = 0; index < capacity; ++index) {
            ugdr::gpu::CopyTask task;
            if (payload.make_task(1000 + index, payload.target_address(), payload_bytes, 0,
                                  &task) != 0 ||
                queue.try_submit(task) != 0) {
                return 65;
            }
        }
        ugdr::gpu::CopyTask overflow_task;
        if (payload.make_task(2000, payload.target_address(), payload_bytes, 0, &overflow_task) !=
                0 ||
            queue.try_submit(overflow_task) != EAGAIN || queue.request_stop() != 0 ||
            queue.wait() != 0 || queue.drained()) {
            return 66;
        }
        for (std::size_t index = 0; index < capacity; ++index) {
            if (queue.try_poll(&completion) != 0 || completion.task_id != 1000 + index) {
                return 67;
            }
        }
        if (!queue.drained()) {
            return 68;
        }
    }
    if (queue.reset() != 0 || !queue.empty()) {
        return 69;
    }
    return 0;
}

int static_partition_head_of_line_smoke() {
    constexpr std::size_t payload_bytes = 8192;
    constexpr std::size_t capacity = 32;
    constexpr std::uint64_t seed = UINT64_C(0x401b10c0401b10c0);

    ugdr::gpu::PersistentCopyPayloadBuffer payload;
    ugdr::gpu::MappedPinnedMemory slow_target;
    if (ugdr::gpu::PersistentCopyPayloadBuffer::allocate(payload_bytes, 16, &payload) != 0 ||
        payload.prepare(seed) != 0 ||
        ugdr::gpu::MappedPinnedMemory::allocate(payload_bytes, &slow_target) != 0) {
        return 70;
    }
    ugdr::gpu::StaticPartitionSpscQueue queue;
    if (ugdr::gpu::StaticPartitionSpscQueue::allocate(capacity, 32, 4, payload.stage_buffer_base(),
                                                      &queue) != 0 ||
        queue.start() != 0) {
        return 71;
    }

    std::vector<ugdr::gpu::CopyTask> tasks(capacity);
    if (payload.make_task(3000, slow_target.device_address(), 1023, 1, &tasks[0]) != 0) {
        return 72;
    }
    for (std::size_t index = 1; index < capacity; ++index) {
        if (payload.make_task(3000 + index, payload.target_address(), 1, 0, &tasks[index]) != 0) {
            return 72;
        }
    }
    std::size_t submitted_count = 0;
    if (queue.try_submit_batch(tasks.data(), tasks.size(), &submitted_count) != 0 ||
        submitted_count != capacity) {
        return 73;
    }

    bool observed_head_of_line = false;
    for (std::size_t attempt = 0; attempt < 1000000; ++attempt) {
        if (queue.head_of_line_blocked()) {
            observed_head_of_line = true;
            break;
        }
    }
    if (!observed_head_of_line || queue.request_stop() != 0 || queue.wait() != 0 ||
        queue.drained()) {
        return 74;
    }
    ugdr::gpu::CopyCompletion completion;
    for (std::size_t index = 0; index < capacity; ++index) {
        if (queue.try_poll(&completion) != 0 || completion.task_id != 3000 + index) {
            return 75;
        }
    }
    if (!queue.drained() || queue.reset() != 0) {
        return 76;
    }
    return 0;
}

int warp_specialized_queue_smoke(std::uint32_t copy_warps, std::uint32_t device_batch,
                                 std::uint32_t shared_queue_depth, bool use_pipeline) {
    constexpr std::size_t payload_bytes = 8192;
    constexpr std::size_t capacity = 32;
    constexpr std::uint64_t seed = UINT64_C(0xa11ce5a6e5a11ce5);
    constexpr std::size_t total_tasks = 257;

    ugdr::gpu::PersistentCopyPayloadBuffer payload;
    if (ugdr::gpu::PersistentCopyPayloadBuffer::allocate(payload_bytes, 16, &payload) != 0 ||
        payload.prepare(seed) != 0) {
        return 78;
    }
    ugdr::gpu::WarpSpecializedQueue queue;
    if (use_pipeline &&
        (ugdr::gpu::WarpSpecializedQueue::allocate_pipeline(
             16, copy_warps, shared_queue_depth, payload.stage_buffer_base(), &queue) != EINVAL ||
         ugdr::gpu::WarpSpecializedQueue::allocate_pipeline(
             capacity, 2, 8, payload.stage_buffer_base(), &queue) != EINVAL)) {
        return 79;
    }
    const std::uint32_t expected_device_batch =
        use_pipeline ? ugdr::gpu::kWarpSpecializedPipelineMetaBatch : device_batch;
    if (ugdr::gpu::WarpSpecializedQueue::allocate(3, 4, 4, 8, payload.stage_buffer_base(),
                                                  &queue) != EINVAL ||
        ugdr::gpu::WarpSpecializedQueue::allocate(capacity, 31, 4, 8, payload.stage_buffer_base(),
                                                  &queue) != EINVAL ||
        ugdr::gpu::WarpSpecializedQueue::allocate(capacity, 4, 3, 8, payload.stage_buffer_base(),
                                                  &queue) != EINVAL ||
        ugdr::gpu::WarpSpecializedQueue::allocate(capacity, 4, 4, 3, payload.stage_buffer_base(),
                                                  &queue) != EINVAL ||
        ugdr::gpu::WarpSpecializedQueue::allocate(capacity, 4, 1, 1, payload.stage_buffer_base(),
                                                  &queue) != EINVAL ||
        ugdr::gpu::WarpSpecializedQueue::allocate(capacity, 4, 4, 1024, payload.stage_buffer_base(),
                                                  &queue) != EINVAL ||
        ugdr::gpu::WarpSpecializedQueue::allocate(capacity, 4, 8, 4, payload.stage_buffer_base(),
                                                  &queue) != EINVAL ||
        (use_pipeline
             ? ugdr::gpu::WarpSpecializedQueue::allocate_pipeline(
                   capacity, copy_warps, shared_queue_depth, payload.stage_buffer_base(), &queue)
             : ugdr::gpu::WarpSpecializedQueue::allocate(
                   capacity, copy_warps, device_batch, shared_queue_depth,
                   payload.stage_buffer_base(), &queue)) != 0 ||
        queue.capacity() != capacity || queue.copy_warps() != copy_warps ||
        queue.device_batch() != expected_device_batch ||
        queue.shared_queue_depth() != shared_queue_depth ||
        queue.host_meta_bytes() != 64 + capacity * 48 ||
        queue.shared_memory_bytes() !=
            shared_queue_depth * 80 * (use_pipeline ? copy_warps : 1) + (use_pipeline ? 0 : 64) ||
        queue.host_system_atomic_operations() != 0 ||
        (use_pipeline ? queue.start_pipeline() : queue.start()) != 0 || !queue.running() ||
        !queue.accepting() ||
        !kernel_resources_match(queue.kernel_resources(), queue.shared_memory_bytes())) {
        return 79;
    }

    ugdr::gpu::CopyCompletion completion;
    std::size_t submitted_count = 99;
    ugdr::gpu::CopyTask invalid_batch[2]{};
    if (payload.make_task(1, payload.target_address(), payload_bytes, 0, &invalid_batch[0]) != 0 ||
        queue.try_poll(&completion) != EAGAIN ||
        queue.try_submit_batch(nullptr, 1, &submitted_count) != EINVAL || submitted_count != 0 ||
        queue.try_submit_batch(nullptr, 0, nullptr) != EINVAL ||
        queue.try_submit_batch(invalid_batch, 2, &submitted_count) != EINVAL ||
        submitted_count != 0 || queue.accepted_tasks() != 0) {
        return 80;
    }

    std::vector<ugdr::gpu::CopyTask> submit_batch(7);
    std::size_t submitted = 0;
    std::size_t completed = 0;
    std::size_t stalled = 0;
    while (completed != total_tasks) {
        bool progressed = false;
        while (submitted != total_tasks) {
            const std::size_t remaining = total_tasks - submitted;
            const std::size_t requested =
                remaining < submit_batch.size() ? remaining : submit_batch.size();
            for (std::size_t index = 0; index < requested; ++index) {
                if (payload.make_task(submitted + index + 1, payload.target_address(),
                                      payload_bytes, 0, &submit_batch[index]) != 0) {
                    return 81;
                }
            }
            submitted_count = 0;
            const int status =
                queue.try_submit_batch(submit_batch.data(), requested, &submitted_count);
            if (status == EAGAIN) {
                break;
            }
            if (status != 0 || submitted_count == 0 || submitted_count > requested) {
                return 82;
            }
            submitted += submitted_count;
            progressed = true;
            if (submitted_count != requested) {
                break;
            }
        }
        while (queue.try_poll(&completion) == 0) {
            if (completion.task_id != completed + 1 ||
                completion.result != ugdr::gpu::CopyTaskResult::success) {
                return 83;
            }
            ++completed;
            progressed = true;
        }
        stalled = progressed ? 0 : stalled + 1;
        if (stalled > 10000000) {
            return 84;
        }
    }

    ugdr::gpu::PayloadCheck check;
    if (!queue.drained() || queue.accepted_tasks() != total_tasks ||
        queue.completed_tasks() != total_tasks || queue.request_stop() != 0 || queue.wait() != 0 ||
        queue.running() || queue.wait() != EINVAL || payload.verify(seed, &check) != 0 ||
        !check.payload_matches || !check.guards_intact) {
        return 85;
    }

    if ((use_pipeline ? queue.start_pipeline() : queue.start()) != 0) {
        return 86;
    }
    for (std::size_t index = 0; index < capacity; ++index) {
        ugdr::gpu::CopyTask task;
        if (payload.make_task(1000 + index, payload.target_address(), payload_bytes, 0, &task) !=
                0 ||
            queue.try_submit(task) != 0) {
            return 87;
        }
    }
    ugdr::gpu::CopyTask overflow_task;
    if (payload.make_task(2000, payload.target_address(), payload_bytes, 0, &overflow_task) != 0 ||
        queue.try_submit(overflow_task) != EAGAIN || queue.request_stop() != 0 ||
        queue.wait() != 0 || queue.drained()) {
        return 88;
    }
    for (std::size_t index = 0; index < capacity; ++index) {
        if (queue.try_poll(&completion) != 0 || completion.task_id != 1000 + index ||
            completion.result != ugdr::gpu::CopyTaskResult::success) {
            return 89;
        }
    }
    if (!queue.drained() || queue.reset() != 0 || !queue.empty()) {
        return 90;
    }
    return 0;
}

}  // namespace

int main() {
    if (!common_contract_smoke()) {
        return 1;
    }
    const int resource_status = gpu_resource_smoke();
    if (resource_status != 0) {
        return resource_status;
    }
    const int copy_core_status = copy_core_matrix_smoke();
    if (copy_core_status != 0) {
        return copy_core_status;
    }
    constexpr std::uint32_t device_batches[]{1, 2, 4, 8, 16, 32};
    for (const std::uint32_t device_batch : device_batches) {
        const int status = direct_atomic_queue_smoke(device_batch);
        if (status != 0) {
            return status;
        }
    }
    for (const std::uint32_t device_batch : device_batches) {
        const int status = dynamic_sharded_spsc_queue_smoke(4, device_batch);
        if (status != 0) {
            return status;
        }
    }
    constexpr std::uint32_t dynamic_copy_warps[]{8, 16, 32};
    for (const std::uint32_t copy_warps : dynamic_copy_warps) {
        const int status = dynamic_sharded_spsc_queue_smoke(copy_warps, 4);
        if (status != 0) {
            return status;
        }
    }
    for (const std::uint32_t device_batch : device_batches) {
        const int status = static_partition_spsc_queue_smoke(4, device_batch);
        if (status != 0) {
            return status;
        }
    }
    for (const std::uint32_t copy_warps : dynamic_copy_warps) {
        const int status = static_partition_spsc_queue_smoke(copy_warps, 4);
        if (status != 0) {
            return status;
        }
    }
    const int head_of_line_status = static_partition_head_of_line_smoke();
    if (head_of_line_status != 0) {
        return head_of_line_status;
    }
    struct WarpSpecializedCase {
        std::uint32_t copy_warps;
        std::uint32_t device_batch;
        std::uint32_t shared_queue_depth;
    };
    constexpr WarpSpecializedCase warp_specialized_cases[]{
        {1, 1, 2},   {4, 2, 2},    {2, 2, 4},    {4, 4, 8},    {6, 8, 16},   {16, 8, 32},
        {10, 8, 64}, {14, 8, 128}, {18, 8, 256}, {22, 8, 512}, {30, 32, 32},
    };
    for (const auto &test_case : warp_specialized_cases) {
        const int status = warp_specialized_queue_smoke(
            test_case.copy_warps, test_case.device_batch, test_case.shared_queue_depth, false);
        if (status != 0) {
            return status;
        }
    }
    struct WarpSpecializedPipelineCase {
        std::uint32_t copy_warps;
        std::uint32_t shared_queue_depth;
    };
    constexpr WarpSpecializedPipelineCase pipeline_cases[]{
        {2, 16}, {6, 8}, {10, 4}, {14, 4}, {18, 2}, {22, 2}, {26, 2}, {30, 2}, {30, 16},
    };
    for (const auto &test_case : pipeline_cases) {
        const int status = warp_specialized_queue_smoke(
            test_case.copy_warps, ugdr::gpu::kWarpSpecializedPipelineMetaBatch,
            test_case.shared_queue_depth, true);
        if (status != 0) {
            return status;
        }
    }
    return 0;
}

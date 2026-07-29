#include "gpu/persistent_copy.hpp"

#include <cuda.h>
#include <cuda_runtime_api.h>
#include <cuda/barrier>

#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace ugdr::gpu {
namespace {

constexpr std::uint8_t kPayloadGuardByte = 0xd3;
constexpr std::uint8_t kPayloadInitialTargetByte = 0xa5;

__device__ __forceinline__ void copy_cg_1(const std::uint8_t *source,
                                          std::uint8_t *target) noexcept {
    std::uint32_t value = 0;
    asm volatile("ld.global.cg.u8 %0, [%1];" : "=r"(value) : "l"(source) : "memory");
    asm volatile("st.global.cg.u8 [%0], %1;" : : "l"(target), "r"(value) : "memory");
}

__device__ __forceinline__ void copy_cg_2(const std::uint8_t *source,
                                          std::uint8_t *target) noexcept {
    std::uint32_t value = 0;
    asm volatile("ld.global.cg.u16 %0, [%1];" : "=r"(value) : "l"(source) : "memory");
    asm volatile("st.global.cg.u16 [%0], %1;" : : "l"(target), "r"(value) : "memory");
}

__device__ __forceinline__ void copy_cg_4(const std::uint8_t *source,
                                          std::uint8_t *target) noexcept {
    std::uint32_t value = 0;
    asm volatile("ld.global.cg.u32 %0, [%1];" : "=r"(value) : "l"(source) : "memory");
    asm volatile("st.global.cg.u32 [%0], %1;" : : "l"(target), "r"(value) : "memory");
}

__device__ __forceinline__ void copy_cg_8(const std::uint8_t *source,
                                          std::uint8_t *target) noexcept {
    std::uint64_t value = 0;
    asm volatile("ld.global.cg.u64 %0, [%1];" : "=l"(value) : "l"(source) : "memory");
    asm volatile("st.global.cg.u64 [%0], %1;" : : "l"(target), "l"(value) : "memory");
}

__device__ __forceinline__ void copy_cg_16(const std::uint8_t *source,
                                           std::uint8_t *target) noexcept {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;
    std::uint32_t w = 0;
    asm volatile("ld.global.cg.v4.u32 {%0, %1, %2, %3}, [%4];"
                 : "=r"(x), "=r"(y), "=r"(z), "=r"(w)
                 : "l"(source)
                 : "memory");
    asm volatile("st.global.cg.v4.u32 [%0], {%1, %2, %3, %4};"
                 :
                 : "l"(target), "r"(x), "r"(y), "r"(z), "r"(w)
                 : "memory");
}

__device__ __forceinline__ void copy_cg_narrow(const std::uint8_t *source,
                                               std::uint8_t *target,
                                               std::uint32_t width) noexcept {
    switch (width) {
    case 8:
        copy_cg_8(source, target);
        break;
    case 4:
        copy_cg_4(source, target);
        break;
    case 2:
        copy_cg_2(source, target);
        break;
    default:
        copy_cg_1(source, target);
        break;
    }
}

__device__ __forceinline__ std::uint32_t largest_safe_narrow_width(
    const std::uint8_t *source, const std::uint8_t *target,
    std::uint32_t remaining) noexcept {
    const std::uintptr_t combined = reinterpret_cast<std::uintptr_t>(source) |
                                    reinterpret_cast<std::uintptr_t>(target);
    for (std::uint32_t width = 8; width != 0; width >>= 1) {
        if (remaining >= width && (combined & (width - 1)) == 0) {
            return width;
        }
    }
    return 1;
}

__device__ __forceinline__ void copy_cg_narrow_serial(const std::uint8_t *source,
                                                      std::uint8_t *target,
                                                      std::uint32_t bytes) noexcept {
    while (bytes != 0) {
        const std::uint32_t width = largest_safe_narrow_width(source, target, bytes);
        copy_cg_narrow(source, target, width);
        source += width;
        target += width;
        bytes -= width;
    }
}

__device__ __forceinline__ std::uint32_t common_bulk_width(
    const std::uint8_t *source, const std::uint8_t *target) noexcept {
    const std::uintptr_t different_bits = reinterpret_cast<std::uintptr_t>(source) ^
                                          reinterpret_cast<std::uintptr_t>(target);
    if ((different_bits & 15U) == 0) {
        return 16;
    }
    if ((different_bits & 7U) == 0) {
        return 8;
    }
    if ((different_bits & 3U) == 0) {
        return 4;
    }
    if ((different_bits & 1U) == 0) {
        return 2;
    }
    return 1;
}

template <std::uint32_t Width>
__device__ __forceinline__ void copy_cg_narrow_bulk_warp(const std::uint8_t *source,
                                                         std::uint8_t *target,
                                                         std::uint32_t unit_count,
                                                         std::uint32_t lane) noexcept {
#pragma unroll 2
    for (std::uint32_t unit = lane; unit < unit_count; unit += 32) {
        const std::uint32_t offset = unit * Width;
        copy_cg_narrow(source + offset, target + offset, Width);
    }
}

__device__ __forceinline__ void copy_cg_16_warp_aligned(const std::uint8_t *source,
                                                        std::uint8_t *target,
                                                        std::uint32_t length) noexcept {
    const std::uint32_t lane = threadIdx.x & 31U;
    const std::uint32_t unit_count = length / 16;
#pragma unroll 2
    for (std::uint32_t unit = lane; unit < unit_count; unit += 32) {
        const std::uint32_t offset = unit * 16;
        copy_cg_16(source + offset, target + offset);
    }
}

__device__ __forceinline__ void copy_cg_warp_generic(const std::uint8_t *source,
                                                      std::uint8_t *target,
                                                      std::uint32_t length) noexcept {
    const std::uint32_t lane = threadIdx.x & 31U;
    const std::uint32_t width = common_bulk_width(source, target);
    const std::uintptr_t source_address = reinterpret_cast<std::uintptr_t>(source);
    const std::uintptr_t misalignment = source_address & (width - 1U);
    std::uint32_t prefix = static_cast<std::uint32_t>(
        (width - misalignment) & (width - 1U));
    if (prefix > length) {
        prefix = length;
    }

    if (lane == 0 && prefix != 0) {
        copy_cg_narrow_serial(source, target, prefix);
    }

    source += prefix;
    target += prefix;
    const std::uint32_t remaining = length - prefix;
    const std::uint32_t unit_count = remaining / width;
    switch (width) {
    case 16:
#pragma unroll 2
        for (std::uint32_t unit = lane; unit < unit_count; unit += 32) {
            const std::uint32_t offset = unit * 16;
            copy_cg_16(source + offset, target + offset);
        }
        break;
    case 8:
        copy_cg_narrow_bulk_warp<8>(source, target, unit_count, lane);
        break;
    case 4:
        copy_cg_narrow_bulk_warp<4>(source, target, unit_count, lane);
        break;
    case 2:
        copy_cg_narrow_bulk_warp<2>(source, target, unit_count, lane);
        break;
    default:
        copy_cg_narrow_bulk_warp<1>(source, target, unit_count, lane);
        break;
    }

    const std::uint32_t body_bytes = unit_count * width;
    const std::uint32_t tail = remaining - body_bytes;
    if (lane == 0 && tail != 0) {
        copy_cg_narrow_serial(source + body_bytes, target + body_bytes, tail);
    }
}

__device__ __forceinline__ void copy_cg_warp(const std::uint8_t *source,
                                             std::uint8_t *target,
                                             std::uint32_t length) noexcept {
    const std::uintptr_t combined = reinterpret_cast<std::uintptr_t>(source) |
                                    reinterpret_cast<std::uintptr_t>(target) | length;
    if ((combined & 15U) == 0) {
        copy_cg_16_warp_aligned(source, target, length);
        return;
    }
    copy_cg_warp_generic(source, target, length);
}

__device__ __forceinline__ void copy_cg_warp_counted(const std::uint8_t *source,
                                                     std::uint8_t *target,
                                                     std::uint32_t length,
                                                     CopyAccessCounts *counts) noexcept {
    const std::uint32_t width = common_bulk_width(source, target);
    const std::uintptr_t source_address = reinterpret_cast<std::uintptr_t>(source);
    const std::uintptr_t misalignment = source_address & (width - 1U);
    std::uint32_t prefix = static_cast<std::uint32_t>(
        (width - misalignment) & (width - 1U));
    if (prefix > length) {
        prefix = length;
    }
    copy_cg_warp(source, target, length);
    __syncwarp();
    if ((threadIdx.x & 31U) == 0) {
        const std::uint64_t vector_bytes =
            width == 16 ? ((length - prefix) / 16) * 16 : 0;
        counts->copied_bytes = length;
        counts->vector_128_bytes = vector_bytes;
        counts->narrow_bytes = length - vector_bytes;
    }
}

__global__ void persistent_copy_core_test_kernel(std::uint64_t stage_buffer_base, CopyTask task,
                                                 std::uint64_t access_counts_address) {
    const auto *const source = reinterpret_cast<const std::uint8_t *>(
        static_cast<std::uintptr_t>(stage_buffer_base + task.relative_offset));
    auto *const target =
        reinterpret_cast<std::uint8_t *>(static_cast<std::uintptr_t>(task.target_address));
    auto *const counts =
        reinterpret_cast<CopyAccessCounts *>(static_cast<std::uintptr_t>(access_counts_address));
    copy_cg_warp_counted(source, target, task.length, counts);
}

struct alignas(64) DirectAtomicControl {
    std::uint64_t task_tail;
    std::uint64_t task_head;
    std::uint64_t completion_tail;
    std::uint64_t stop_requested;
};

struct alignas(32) DirectAtomicTaskSlot {
    CopyTask task;
};

struct alignas(32) DirectAtomicCompletionSlot {
    CopyCompletion completion;
    std::uint64_t sequence;
    std::uint64_t batch_start;
};

struct alignas(64) DynamicShardedSpscControl {
    std::uint64_t task_tail;
    std::uint64_t completion_tail;
    std::uint64_t stop_requested;
};

struct alignas(32) DynamicShardedSpscTaskSlot {
    CopyTask task;
};

struct alignas(16) DynamicShardedSpscCompletionSlot {
    CopyCompletion completion;
};

struct alignas(64) StaticPartitionSpscControl {
    std::uint64_t task_tail;
    std::uint64_t stop_requested;
};

struct alignas(32) StaticPartitionSpscTaskSlot {
    CopyTask task;
};

struct alignas(32) StaticPartitionSpscCompletionSlot {
    CopyCompletion completion;
    std::uint64_t sequence;
};

struct alignas(64) WarpSpecializedControl {
    std::uint64_t task_tail;
    std::uint64_t completion_tail;
    std::uint64_t stop_requested;
};

struct alignas(32) WarpSpecializedTaskSlot {
    CopyTask task;
};

struct alignas(16) WarpSpecializedCompletionSlot {
    CopyCompletion completion;
};

struct alignas(64) WarpSpecializedSharedControl {
    std::uint64_t task_publish;
    std::uint64_t task_claim;
    std::uint64_t ingress_done;
};

struct alignas(16) WarpSpecializedSharedTaskSlot {
    CopyTask task;
    std::uint64_t sequence;
};

struct alignas(16) WarpSpecializedSharedCompletionSlot {
    CopyCompletion completion;
    std::uint64_t sequence;
};

using BlockBarrier =
    cuda::barrier<cuda::thread_scope_block>;

struct alignas(16) WarpSpecializedPipelineTaskSlot {
    CopyTask task;
    BlockBarrier ready;
    BlockBarrier consumed;
};

struct alignas(16) WarpSpecializedPipelineCompletionSlot {
    CopyCompletion completion;
    BlockBarrier ready;
    BlockBarrier consumed;
};

static_assert(sizeof(DirectAtomicControl) == 64);
static_assert(sizeof(DirectAtomicTaskSlot) == 32);
static_assert(sizeof(DirectAtomicCompletionSlot) == 32);
static_assert(sizeof(DynamicShardedSpscControl) == 64);
static_assert(sizeof(DynamicShardedSpscTaskSlot) == 32);
static_assert(sizeof(DynamicShardedSpscCompletionSlot) == 16);
static_assert(sizeof(StaticPartitionSpscControl) == 64);
static_assert(sizeof(StaticPartitionSpscTaskSlot) == 32);
static_assert(sizeof(StaticPartitionSpscCompletionSlot) == 32);
static_assert(sizeof(WarpSpecializedControl) == 64);
static_assert(sizeof(WarpSpecializedTaskSlot) == 32);
static_assert(sizeof(WarpSpecializedCompletionSlot) == 16);
static_assert(sizeof(WarpSpecializedSharedControl) == 64);
static_assert(sizeof(WarpSpecializedSharedTaskSlot) == 48);
static_assert(sizeof(WarpSpecializedSharedCompletionSlot) == 32);
static_assert(sizeof(WarpSpecializedPipelineTaskSlot) == 48);
static_assert(
    sizeof(WarpSpecializedPipelineCompletionSlot) == 32);

__device__ __forceinline__ std::uint64_t system_load_acquire(
    const std::uint64_t *address) noexcept {
    std::uint64_t value = 0;
    asm volatile("ld.acquire.sys.global.u64 %0, [%1];"
                 : "=l"(value)
                 : "l"(address)
                 : "memory");
    return value;
}

__device__ __forceinline__ void system_store_release(std::uint64_t *address,
                                                     std::uint64_t value) noexcept {
    asm volatile("st.release.sys.global.u64 [%0], %1;"
                 :
                 : "l"(address), "l"(value)
                 : "memory");
}

__device__ __forceinline__ std::uint64_t system_atomic_cas(std::uint64_t *address,
                                                           std::uint64_t expected,
                                                           std::uint64_t desired) noexcept {
    std::uint64_t observed = 0;
    asm volatile("atom.acquire.sys.global.cas.b64 %0, [%1], %2, %3;"
                 : "=l"(observed)
                 : "l"(address), "l"(expected), "l"(desired)
                 : "memory");
    return observed;
}

__device__ __forceinline__ std::uint64_t system_atomic_add(std::uint64_t *address,
                                                           std::uint64_t value) noexcept {
    std::uint64_t previous = 0;
    asm volatile("atom.relaxed.sys.global.add.u64 %0, [%1], %2;"
                 : "=l"(previous)
                 : "l"(address), "l"(value)
                 : "memory");
    return previous;
}

__device__ __forceinline__ std::uint64_t shared_atomic_load(
    std::uint64_t *address) noexcept {
    return atomicAdd(reinterpret_cast<unsigned long long *>(address), 0ULL);
}

__device__ __forceinline__ void shared_atomic_store(
    std::uint64_t *address, std::uint64_t value) noexcept {
    (void)atomicExch(reinterpret_cast<unsigned long long *>(address),
                     static_cast<unsigned long long>(value));
}

__device__ __forceinline__ bool shared_atomic_claim(
    std::uint64_t *address, std::uint64_t expected) noexcept {
    return atomicCAS(reinterpret_cast<unsigned long long *>(address),
                     static_cast<unsigned long long>(expected),
                     static_cast<unsigned long long>(expected + 1)) ==
           expected;
}

template <std::uint32_t TasksPerClaim>
__global__ void direct_atomic_kernel(DirectAtomicControl *control,
                                     DirectAtomicTaskSlot *task_slots,
                                     DirectAtomicCompletionSlot *completion_slots,
                                     std::uint64_t capacity_mask,
                                     std::uint64_t stage_buffer_base) {
    static_assert(TasksPerClaim > 0);
    constexpr std::uint64_t kNoTask = UINT64_MAX;
    constexpr unsigned kFullWarp = 0xffffffffU;
    const std::uint32_t lane = threadIdx.x & 31U;

    while (true) {
        std::uint64_t task_base = kNoTask;
        std::uint32_t task_count = 0;
        bool should_stop = false;
        if (lane == 0) {
            const std::uint64_t tail = system_load_acquire(&control->task_tail);
            const std::uint64_t head = system_load_acquire(&control->task_head);
            if (head < tail) {
                const std::uint64_t available = tail - head;
                task_count = static_cast<std::uint32_t>(
                    available < TasksPerClaim ? available : TasksPerClaim);
                if (system_atomic_cas(&control->task_head, head, head + task_count) == head) {
                    task_base = head;
                } else {
                    task_count = 0;
                }
            } else if (head >= tail &&
                       system_load_acquire(&control->stop_requested) != 0) {
                const std::uint64_t final_tail =
                    system_load_acquire(&control->task_tail);
                const std::uint64_t final_head =
                    system_load_acquire(&control->task_head);
                should_stop = final_head >= final_tail;
            }
        }
        task_base = __shfl_sync(kFullWarp, task_base, 0);
        task_count = __shfl_sync(kFullWarp, task_count, 0);
        should_stop = __shfl_sync(kFullWarp, should_stop, 0);
        if (should_stop) {
            return;
        }
        if (task_base == kNoTask) {
            if (lane == 0) {
                __nanosleep(64);
            }
            continue;
        }

        std::uint64_t completion_base = 0;
        if (lane == 0) {
            completion_base = system_atomic_add(&control->completion_tail, task_count);
        }

#pragma unroll 1
        for (std::uint32_t batch_index = 0; batch_index < task_count; ++batch_index) {
            const std::uint64_t task_index = task_base + batch_index;
            DirectAtomicTaskSlot *const task_slot = &task_slots[task_index & capacity_mask];
            CopyTask task{};
            if (lane == 0) {
                task = task_slot->task;
            }
            task.target_address = __shfl_sync(kFullWarp, task.target_address, 0);
            task.length = __shfl_sync(kFullWarp, task.length, 0);
            task.relative_offset = __shfl_sync(kFullWarp, task.relative_offset, 0);

            const auto *const source = reinterpret_cast<const std::uint8_t *>(
                static_cast<std::uintptr_t>(stage_buffer_base + task.relative_offset));
            auto *const target =
                reinterpret_cast<std::uint8_t *>(static_cast<std::uintptr_t>(task.target_address));
            copy_cg_warp(source, target, task.length);
            __syncwarp();

            if (lane == 0) {
                const std::uint64_t completion_index = completion_base + batch_index;
                DirectAtomicCompletionSlot *const completion_slot =
                    &completion_slots[completion_index & capacity_mask];
                completion_slot->completion.task_id = task.task_id;
                completion_slot->completion.result = CopyTaskResult::success;
                completion_slot->batch_start = batch_index == 0 ? 1 : 0;
                system_store_release(&completion_slot->sequence, completion_index + 1);
            }
        }
    }
}

template <std::uint32_t TasksPerBatch>
__global__ void dynamic_sharded_spsc_kernel(
    DynamicShardedSpscControl *controls, DynamicShardedSpscTaskSlot *task_slots,
    DynamicShardedSpscCompletionSlot *completion_slots, std::uint64_t lane_capacity,
    std::uint64_t lane_capacity_mask, std::uint64_t stage_buffer_base) {
    static_assert(TasksPerBatch > 0);
    constexpr unsigned kFullWarp = 0xffffffffU;
    const std::uint32_t lane = threadIdx.x & 31U;
    const std::uint32_t warp = threadIdx.x >> 5U;
    DynamicShardedSpscControl *const control = &controls[warp];
    const std::uint64_t slot_base = static_cast<std::uint64_t>(warp) * lane_capacity;
    std::uint64_t task_head = 0;

    while (true) {
        std::uint64_t task_tail = 0;
        std::uint32_t task_count = 0;
        bool should_stop = false;
        if (lane == 0) {
            task_tail = system_load_acquire(&control->task_tail);
            if (task_head < task_tail) {
                const std::uint64_t available = task_tail - task_head;
                task_count = static_cast<std::uint32_t>(
                    available < TasksPerBatch ? available : TasksPerBatch);
            } else if (system_load_acquire(&control->stop_requested) != 0) {
                const std::uint64_t final_tail =
                    system_load_acquire(&control->task_tail);
                should_stop = task_head >= final_tail;
                if (!should_stop) {
                    const std::uint64_t available = final_tail - task_head;
                    task_count = static_cast<std::uint32_t>(
                        available < TasksPerBatch ? available : TasksPerBatch);
                }
            }
        }
        task_count = __shfl_sync(kFullWarp, task_count, 0);
        should_stop = __shfl_sync(kFullWarp, should_stop, 0);
        if (should_stop) {
            return;
        }
        if (task_count == 0) {
            if (lane == 0) {
                __nanosleep(64);
            }
            continue;
        }

#pragma unroll 1
        for (std::uint32_t batch_index = 0; batch_index < task_count;
             ++batch_index) {
            const std::uint64_t slot_index =
                slot_base +
                ((task_head + batch_index) & lane_capacity_mask);
            DynamicShardedSpscTaskSlot *const task_slot =
                &task_slots[slot_index];
            CopyTask task{};
            if (lane == 0) {
                task = task_slot->task;
            }
            task.target_address =
                __shfl_sync(kFullWarp, task.target_address, 0);
            task.length = __shfl_sync(kFullWarp, task.length, 0);
            task.relative_offset =
                __shfl_sync(kFullWarp, task.relative_offset, 0);

            const auto *const source =
                reinterpret_cast<const std::uint8_t *>(
                    static_cast<std::uintptr_t>(
                        stage_buffer_base + task.relative_offset));
            auto *const target = reinterpret_cast<std::uint8_t *>(
                static_cast<std::uintptr_t>(task.target_address));
            copy_cg_warp(source, target, task.length);
            __syncwarp();

            if (lane == 0) {
                DynamicShardedSpscCompletionSlot *const completion_slot =
                    &completion_slots[slot_index];
                completion_slot->completion.task_id = task.task_id;
                completion_slot->completion.result =
                    CopyTaskResult::success;
            }
        }
        task_head += task_count;
        if (lane == 0) {
            system_store_release(&control->completion_tail, task_head);
        }
    }
}

template <std::uint32_t TasksPerBatch>
__global__ void static_partition_spsc_kernel(
    StaticPartitionSpscControl *control,
    StaticPartitionSpscTaskSlot *task_slots,
    StaticPartitionSpscCompletionSlot *completion_slots,
    std::uint64_t capacity_mask, std::uint32_t copy_warps,
    std::uint32_t copy_warp_shift, std::uint64_t stage_buffer_base) {
    static_assert(TasksPerBatch > 0);
    static_assert(TasksPerBatch <= 32);
    constexpr unsigned kFullWarp = 0xffffffffU;
    const std::uint32_t lane = threadIdx.x & 31U;
    const std::uint32_t warp = threadIdx.x >> 5U;
    std::uint64_t owned_index = warp;

    while (true) {
        std::uint32_t task_count = 0;
        bool should_stop = false;
        if (lane == 0) {
            std::uint64_t task_tail =
                system_load_acquire(&control->task_tail);
            if (owned_index >= task_tail &&
                system_load_acquire(&control->stop_requested) != 0) {
                task_tail = system_load_acquire(&control->task_tail);
                should_stop = owned_index >= task_tail;
            }
            if (!should_stop && owned_index < task_tail) {
                const std::uint64_t available =
                    ((task_tail - 1 - owned_index) >> copy_warp_shift) + 1;
                task_count = static_cast<std::uint32_t>(
                    available < TasksPerBatch ? available : TasksPerBatch);
            }
        }
        task_count = __shfl_sync(kFullWarp, task_count, 0);
        should_stop = __shfl_sync(kFullWarp, should_stop, 0);
        if (should_stop) {
            return;
        }
        if (task_count == 0) {
            if (lane == 0) {
                __nanosleep(64);
            }
            continue;
        }

        const std::uint64_t lane_task_index =
            owned_index + static_cast<std::uint64_t>(lane) * copy_warps;
        CopyTask lane_task{};
        if (lane < task_count) {
            lane_task =
                task_slots[lane_task_index & capacity_mask].task;
        }
        __syncwarp();

#pragma unroll 1
        for (std::uint32_t batch_index = 0; batch_index < task_count;
             ++batch_index) {
            CopyTask task{};
            task.target_address = __shfl_sync(
                kFullWarp, lane_task.target_address, batch_index);
            task.length = __shfl_sync(
                kFullWarp, lane_task.length, batch_index);
            task.relative_offset = __shfl_sync(
                kFullWarp, lane_task.relative_offset, batch_index);
            const auto *const source =
                reinterpret_cast<const std::uint8_t *>(
                    static_cast<std::uintptr_t>(
                        stage_buffer_base + task.relative_offset));
            auto *const target = reinterpret_cast<std::uint8_t *>(
                static_cast<std::uintptr_t>(task.target_address));
            copy_cg_warp(source, target, task.length);
            __syncwarp();
        }

        if (lane < task_count) {
            StaticPartitionSpscCompletionSlot *const completion_slot =
                &completion_slots[lane_task_index & capacity_mask];
            completion_slot->completion.task_id = lane_task.task_id;
            completion_slot->completion.result =
                CopyTaskResult::success;
            system_store_release(&completion_slot->sequence,
                                 lane_task_index + 1);
        }
        owned_index +=
            static_cast<std::uint64_t>(task_count) * copy_warps;
    }
}

template <std::uint32_t TasksPerBatch,
          std::uint32_t SharedQueueDepth>
__global__ void warp_specialized_kernel(
    WarpSpecializedControl *control,
    WarpSpecializedTaskSlot *external_task_slots,
    WarpSpecializedCompletionSlot *external_completion_slots,
    std::uint64_t capacity_mask, std::uint32_t copy_warps,
    std::uint64_t stage_buffer_base) {
    static_assert(TasksPerBatch > 0);
    static_assert(TasksPerBatch <= 32);
    static_assert(SharedQueueDepth >= 2);
    static_assert(SharedQueueDepth <= 512);
    static_assert(
        (SharedQueueDepth & (SharedQueueDepth - 1)) == 0);
    static_assert(TasksPerBatch <= SharedQueueDepth);
    constexpr std::uint64_t kSharedQueueMask =
        SharedQueueDepth - 1;
    extern __shared__ __align__(64) std::uint8_t shared_memory[];
    auto *const shared_control =
        reinterpret_cast<WarpSpecializedSharedControl *>(shared_memory);
    auto *const shared_task_slots =
        reinterpret_cast<WarpSpecializedSharedTaskSlot *>(
            shared_control + 1);
    auto *const shared_completion_slots =
        reinterpret_cast<WarpSpecializedSharedCompletionSlot *>(
            shared_task_slots + SharedQueueDepth);

    constexpr unsigned kFullWarp = 0xffffffffU;
    constexpr std::uint64_t kNoTask = UINT64_MAX;
    const std::uint32_t lane = threadIdx.x & 31U;
    const std::uint32_t warp = threadIdx.x >> 5U;
    for (std::uint64_t index = threadIdx.x;
         index < SharedQueueDepth; index += blockDim.x) {
        shared_task_slots[index].sequence = index;
        shared_completion_slots[index].sequence = index;
    }
    if (threadIdx.x == 0) {
        shared_control->task_publish = 0;
        shared_control->task_claim = 0;
        shared_control->ingress_done = 0;
    }
    __syncthreads();

    if (warp == 0) {
        std::uint64_t task_head = 0;
        while (true) {
            std::uint32_t task_count = 0;
            bool should_stop = false;
            if (lane == 0) {
                std::uint64_t task_tail =
                    system_load_acquire(&control->task_tail);
                if (task_head < task_tail) {
                    const std::uint64_t available =
                        task_tail - task_head;
                    task_count = static_cast<std::uint32_t>(
                        available < TasksPerBatch
                            ? available
                            : TasksPerBatch);
                } else if (
                    system_load_acquire(&control->stop_requested) != 0) {
                    task_tail =
                        system_load_acquire(&control->task_tail);
                    should_stop = task_head >= task_tail;
                }
            }
            task_count =
                __shfl_sync(kFullWarp, task_count, 0);
            should_stop =
                __shfl_sync(kFullWarp, should_stop, 0);
            if (should_stop) {
                if (lane == 0) {
                    shared_atomic_store(
                        &shared_control->ingress_done, 1);
                }
                return;
            }
            if (task_count == 0) {
                if (lane == 0) {
                    __nanosleep(64);
                }
                continue;
            }

            if (lane < task_count) {
                const std::uint64_t task_index =
                    task_head + lane;
                auto *const shared_slot =
                    &shared_task_slots[
                        task_index & kSharedQueueMask];
                while (shared_atomic_load(
                           &shared_slot->sequence) !=
                       task_index) {
                    __nanosleep(64);
                }
                shared_slot->task =
                    external_task_slots[
                        task_index & capacity_mask].task;
                __threadfence_block();
                shared_atomic_store(
                    &shared_slot->sequence, task_index + 1);
            }
            __syncwarp();
            task_head += task_count;
            if (lane == 0) {
                shared_atomic_store(
                    &shared_control->task_publish, task_head);
            }
        }
    }

    if (warp <= copy_warps) {
        while (true) {
            std::uint64_t task_index = kNoTask;
            bool should_stop = false;
            if (lane == 0) {
                const std::uint64_t task_claim =
                    shared_atomic_load(
                        &shared_control->task_claim);
                const std::uint64_t task_publish =
                    shared_atomic_load(
                        &shared_control->task_publish);
                if (task_claim < task_publish) {
                    if (shared_atomic_claim(
                            &shared_control->task_claim,
                            task_claim)) {
                        task_index = task_claim;
                    }
                } else if (
                    shared_atomic_load(
                        &shared_control->ingress_done) != 0) {
                    should_stop = task_claim >= task_publish;
                }
            }
            task_index =
                __shfl_sync(kFullWarp, task_index, 0);
            should_stop =
                __shfl_sync(kFullWarp, should_stop, 0);
            if (should_stop) {
                return;
            }
            if (task_index == kNoTask) {
                if (lane == 0) {
                    __nanosleep(64);
                }
                continue;
            }

            WarpSpecializedSharedTaskSlot *const task_slot =
                &shared_task_slots[
                    task_index & kSharedQueueMask];
            CopyTask task{};
            if (lane == 0) {
                while (shared_atomic_load(
                           &task_slot->sequence) != task_index + 1) {
                    __nanosleep(64);
                }
                task = task_slot->task;
            }
            task.target_address = __shfl_sync(
                kFullWarp, task.target_address, 0);
            task.length =
                __shfl_sync(kFullWarp, task.length, 0);
            task.relative_offset = __shfl_sync(
                kFullWarp, task.relative_offset, 0);
            const auto *const source =
                reinterpret_cast<const std::uint8_t *>(
                    static_cast<std::uintptr_t>(
                        stage_buffer_base +
                        task.relative_offset));
            auto *const target =
                reinterpret_cast<std::uint8_t *>(
                    static_cast<std::uintptr_t>(
                        task.target_address));
            copy_cg_warp(source, target, task.length);
            __syncwarp();

            if (lane == 0) {
                shared_atomic_store(
                    &task_slot->sequence,
                    task_index + SharedQueueDepth);
                WarpSpecializedSharedCompletionSlot *const
                    completion_slot =
                        &shared_completion_slots[
                            task_index & kSharedQueueMask];
                while (shared_atomic_load(
                           &completion_slot->sequence) !=
                       task_index) {
                    __nanosleep(64);
                }
                completion_slot->completion.task_id =
                    task.task_id;
                completion_slot->completion.result =
                    CopyTaskResult::success;
                __threadfence_block();
                shared_atomic_store(
                    &completion_slot->sequence,
                    task_index + 1);
            }
        }
    }

    std::uint64_t completion_head = 0;
    while (true) {
        std::uint32_t completion_limit = 0;
        std::uint32_t completion_count = 0;
        bool should_stop = false;
        if (lane == 0) {
            const std::uint64_t task_publish =
                shared_atomic_load(
                    &shared_control->task_publish);
            const std::uint64_t available =
                task_publish - completion_head;
            completion_limit =
                static_cast<std::uint32_t>(
                    available < TasksPerBatch
                        ? available
                        : TasksPerBatch);
            should_stop =
                shared_atomic_load(
                    &shared_control->ingress_done) != 0 &&
                completion_head >= task_publish;
        }
        completion_limit =
            __shfl_sync(kFullWarp, completion_limit, 0);
        should_stop =
            __shfl_sync(kFullWarp, should_stop, 0);
        if (should_stop) {
            return;
        }

        bool completion_ready = false;
        if (lane < completion_limit) {
            const std::uint64_t completion_index =
                completion_head + lane;
            completion_ready =
                shared_atomic_load(
                    &shared_completion_slots[
                        completion_index &
                        kSharedQueueMask].sequence) ==
                completion_index + 1;
        }
        const unsigned ready_mask =
            __ballot_sync(kFullWarp, completion_ready);
        if (lane == 0) {
            unsigned expected_mask = kFullWarp;
            if (completion_limit < 32) {
                expected_mask =
                    (1U << completion_limit) - 1U;
            }
            const unsigned missing_mask =
                expected_mask & ~ready_mask;
            completion_count =
                missing_mask == 0
                    ? completion_limit
                    : static_cast<std::uint32_t>(
                          __ffs(missing_mask) - 1);
        }
        completion_count =
            __shfl_sync(kFullWarp, completion_count, 0);
        if (completion_count == 0) {
            if (lane == 0) {
                __nanosleep(64);
            }
            continue;
        }

        if (lane < completion_count) {
            const std::uint64_t completion_index =
                completion_head + lane;
            external_completion_slots[
                completion_index & capacity_mask].completion =
                shared_completion_slots[
                    completion_index &
                    kSharedQueueMask].completion;
        }
        __syncwarp();
        if (lane == 0) {
            system_store_release(
                &control->completion_tail,
                completion_head + completion_count);
        }
        if (lane < completion_count) {
            const std::uint64_t completion_index =
                completion_head + lane;
            shared_atomic_store(
                &shared_completion_slots[
                    completion_index &
                    kSharedQueueMask].sequence,
                completion_index + SharedQueueDepth);
        }
        __syncwarp();
        completion_head += completion_count;
    }
}

template <std::uint32_t TasksPerBatch,
          std::uint32_t SharedQueueDepth>
__global__ void warp_specialized_pipeline_kernel(
    WarpSpecializedControl *control,
    WarpSpecializedTaskSlot *external_task_slots,
    WarpSpecializedCompletionSlot *external_completion_slots,
    std::uint64_t capacity_mask, std::uint32_t copy_warps,
    std::uint64_t stage_buffer_base) {
    static_assert(TasksPerBatch > 0);
    static_assert(TasksPerBatch <= 32);
    static_assert(SharedQueueDepth >= 2);
    static_assert(SharedQueueDepth <= 512);
    static_assert(
        (SharedQueueDepth & (SharedQueueDepth - 1)) == 0);
    static_assert(TasksPerBatch <= SharedQueueDepth);
    constexpr std::uint64_t kSharedQueueMask =
        SharedQueueDepth - 1;
    constexpr unsigned kFullWarp = 0xffffffffU;

    extern __shared__ __align__(64) std::uint8_t shared_memory[];
    auto *const shared_task_slots =
        reinterpret_cast<WarpSpecializedPipelineTaskSlot *>(
            shared_memory + 64);
    const std::uint32_t shared_slot_count =
        SharedQueueDepth * copy_warps;
    auto *const shared_completion_slots =
        reinterpret_cast<
            WarpSpecializedPipelineCompletionSlot *>(
            shared_task_slots + shared_slot_count);

    const std::uint32_t lane = threadIdx.x & 31U;
    const std::uint32_t warp = threadIdx.x >> 5U;
    for (std::uint64_t index = threadIdx.x;
         index < shared_slot_count; index += blockDim.x) {
        init(&shared_task_slots[index].ready, 1);
        init(&shared_task_slots[index].consumed, 1);
        init(&shared_completion_slots[index].ready, 1);
        init(
            &shared_completion_slots[index].consumed, 1);
    }
    __syncthreads();

    if (warp == 0) {
        std::uint64_t task_head = 0;
        while (true) {
            std::uint32_t task_count = 0;
            bool should_stop = false;
            if (lane == 0) {
                std::uint64_t task_tail =
                    system_load_acquire(&control->task_tail);
                if (task_head < task_tail) {
                    const std::uint64_t available =
                        task_tail - task_head;
                    task_count = static_cast<std::uint32_t>(
                        available < TasksPerBatch
                            ? available
                            : TasksPerBatch);
                } else if (
                    system_load_acquire(
                        &control->stop_requested) != 0) {
                    task_tail =
                        system_load_acquire(
                            &control->task_tail);
                    should_stop = task_head >= task_tail;
                }
            }
            task_count =
                __shfl_sync(kFullWarp, task_count, 0);
            should_stop =
                __shfl_sync(kFullWarp, should_stop, 0);
            if (should_stop) {
                if (lane < copy_warps) {
                    const std::uint64_t task_index =
                        task_head + lane;
                    const std::uint64_t owner =
                        task_index % copy_warps;
                    const std::uint64_t local_index =
                        task_index / copy_warps;
                    const std::uint64_t slot_index =
                        owner * SharedQueueDepth +
                        (local_index & kSharedQueueMask);
                    auto *const slot =
                        &shared_task_slots[
                            slot_index];
                    const bool phase =
                        ((local_index / SharedQueueDepth) &
                         1U) != 0;
                    slot->consumed.wait_parity(!phase);
                    slot->task = {};
                    (void)slot->ready.arrive();
                }
                return;
            }
            if (task_count == 0) {
                if (lane == 0) {
                    __nanosleep(64);
                }
                continue;
            }

            if (lane < task_count) {
                const std::uint64_t task_index =
                    task_head + lane;
                const std::uint64_t owner =
                    task_index % copy_warps;
                const std::uint64_t local_index =
                    task_index / copy_warps;
                const std::uint64_t slot_index =
                    owner * SharedQueueDepth +
                    (local_index & kSharedQueueMask);
                auto *const slot =
                    &shared_task_slots[slot_index];
                const bool phase =
                    ((local_index / SharedQueueDepth) &
                     1U) != 0;
                slot->consumed.wait_parity(!phase);
                slot->task =
                    external_task_slots[
                        task_index & capacity_mask].task;
                (void)slot->ready.arrive();
            }
            __syncwarp();
            task_head += task_count;
        }
    }

    if (warp <= copy_warps) {
        const std::uint64_t owner = warp - 1;
        std::uint64_t local_index = 0;
        while (true) {
            const std::uint64_t slot_index =
                owner * SharedQueueDepth +
                (local_index & kSharedQueueMask);
            auto *const task_slot =
                &shared_task_slots[slot_index];
            const bool phase =
                ((local_index / SharedQueueDepth) & 1U) != 0;
            CopyTask task{};
            if (lane == 0) {
                task_slot->ready.wait_parity(phase);
                task = task_slot->task;
                (void)task_slot->consumed.arrive();
            }
            task.task_id =
                __shfl_sync(kFullWarp, task.task_id, 0);
            task.target_address = __shfl_sync(
                kFullWarp, task.target_address, 0);
            task.length =
                __shfl_sync(kFullWarp, task.length, 0);
            task.relative_offset = __shfl_sync(
                kFullWarp, task.relative_offset, 0);
            if (task.length == 0) {
                return;
            }

            const auto *const source =
                reinterpret_cast<const std::uint8_t *>(
                    static_cast<std::uintptr_t>(
                        stage_buffer_base +
                        task.relative_offset));
            auto *const target =
                reinterpret_cast<std::uint8_t *>(
                    static_cast<std::uintptr_t>(
                        task.target_address));
            copy_cg_warp(source, target, task.length);
            __syncwarp();

            if (lane == 0) {
                auto *const completion_slot =
                    &shared_completion_slots[slot_index];
                completion_slot->consumed.wait_parity(
                    !phase);
                completion_slot->completion.task_id =
                    task.task_id;
                completion_slot->completion.result =
                    CopyTaskResult::success;
                (void)completion_slot->ready.arrive();
            }
            ++local_index;
        }
    }

    std::uint64_t completion_head = 0;
    while (true) {
        std::uint32_t completion_count = 0;
        bool should_stop = false;
        if (lane == 0) {
            std::uint64_t task_tail =
                system_load_acquire(&control->task_tail);
            if (completion_head < task_tail) {
                const std::uint64_t available =
                    task_tail - completion_head;
                completion_count =
                    static_cast<std::uint32_t>(
                        available < TasksPerBatch
                            ? available
                            : TasksPerBatch);
            } else if (
                system_load_acquire(
                    &control->stop_requested) != 0) {
                task_tail =
                    system_load_acquire(
                        &control->task_tail);
                should_stop =
                    completion_head >= task_tail;
            }
        }
        completion_count =
            __shfl_sync(
                kFullWarp, completion_count, 0);
        should_stop =
            __shfl_sync(kFullWarp, should_stop, 0);
        if (should_stop) {
            return;
        }
        if (completion_count == 0) {
            if (lane == 0) {
                __nanosleep(64);
            }
            continue;
        }

        if (lane < completion_count) {
            const std::uint64_t completion_index =
                completion_head + lane;
            const std::uint64_t owner =
                completion_index % copy_warps;
            const std::uint64_t local_index =
                completion_index / copy_warps;
            const std::uint64_t slot_index =
                owner * SharedQueueDepth +
                (local_index & kSharedQueueMask);
            auto *const slot =
                &shared_completion_slots[slot_index];
            const bool phase =
                ((local_index / SharedQueueDepth) &
                 1U) != 0;
            slot->ready.wait_parity(phase);
            external_completion_slots[
                completion_index &
                capacity_mask].completion =
                slot->completion;
        }
        __syncwarp();
        if (lane == 0) {
            system_store_release(
                &control->completion_tail,
                completion_head + completion_count);
        }
        if (lane < completion_count) {
            const std::uint64_t completion_index =
                completion_head + lane;
            const std::uint64_t owner =
                completion_index % copy_warps;
            const std::uint64_t local_index =
                completion_index / copy_warps;
            const std::uint64_t slot_index =
                owner * SharedQueueDepth +
                (local_index & kSharedQueueMask);
            (void)shared_completion_slots[
                slot_index].consumed.arrive();
        }
        __syncwarp();
        completion_head += completion_count;
    }
}

template <std::uint32_t TasksPerBatch,
          std::uint32_t SharedQueueDepth>
void launch_warp_specialized_kernel(
    WarpSpecializedControl *control,
    WarpSpecializedTaskSlot *task_slots,
    WarpSpecializedCompletionSlot *completion_slots,
    std::uint64_t capacity_mask, std::uint32_t copy_warps,
    std::uint64_t stage_buffer_base,
    std::uint32_t thread_count,
    std::size_t dynamic_shared_memory_bytes) {
    warp_specialized_kernel<TasksPerBatch, SharedQueueDepth>
        <<<1, thread_count, dynamic_shared_memory_bytes>>>(
            control, task_slots, completion_slots,
            capacity_mask, copy_warps, stage_buffer_base);
}

template <std::uint32_t SharedQueueDepth>
bool dispatch_warp_specialized_batch(
    std::uint32_t device_batch,
    WarpSpecializedControl *control,
    WarpSpecializedTaskSlot *task_slots,
    WarpSpecializedCompletionSlot *completion_slots,
    std::uint64_t capacity_mask, std::uint32_t copy_warps,
    std::uint64_t stage_buffer_base,
    std::uint32_t thread_count,
    std::size_t dynamic_shared_memory_bytes) {
    switch (device_batch) {
    case 1:
        launch_warp_specialized_kernel<1, SharedQueueDepth>(
            control, task_slots, completion_slots,
            capacity_mask, copy_warps, stage_buffer_base,
            thread_count, dynamic_shared_memory_bytes);
        return true;
    case 2:
        if constexpr (SharedQueueDepth >= 2) {
            launch_warp_specialized_kernel<2, SharedQueueDepth>(
                control, task_slots, completion_slots,
                capacity_mask, copy_warps, stage_buffer_base,
                thread_count, dynamic_shared_memory_bytes);
            return true;
        }
        return false;
    case 4:
        if constexpr (SharedQueueDepth >= 4) {
            launch_warp_specialized_kernel<4, SharedQueueDepth>(
                control, task_slots, completion_slots,
                capacity_mask, copy_warps, stage_buffer_base,
                thread_count, dynamic_shared_memory_bytes);
            return true;
        }
        return false;
    case 8:
        if constexpr (SharedQueueDepth >= 8) {
            launch_warp_specialized_kernel<8, SharedQueueDepth>(
                control, task_slots, completion_slots,
                capacity_mask, copy_warps, stage_buffer_base,
                thread_count, dynamic_shared_memory_bytes);
            return true;
        }
        return false;
    case 16:
        if constexpr (SharedQueueDepth >= 16) {
            launch_warp_specialized_kernel<16, SharedQueueDepth>(
                control, task_slots, completion_slots,
                capacity_mask, copy_warps, stage_buffer_base,
                thread_count, dynamic_shared_memory_bytes);
            return true;
        }
        return false;
    case 32:
        if constexpr (SharedQueueDepth >= 32) {
            launch_warp_specialized_kernel<32, SharedQueueDepth>(
                control, task_slots, completion_slots,
                capacity_mask, copy_warps, stage_buffer_base,
                thread_count, dynamic_shared_memory_bytes);
            return true;
        }
        return false;
    default:
        return false;
    }
}

template <std::uint32_t TasksPerBatch,
          std::uint32_t SharedQueueDepth>
void launch_warp_specialized_pipeline_kernel(
    WarpSpecializedControl *control,
    WarpSpecializedTaskSlot *task_slots,
    WarpSpecializedCompletionSlot *completion_slots,
    std::uint64_t capacity_mask, std::uint32_t copy_warps,
    std::uint64_t stage_buffer_base,
    std::uint32_t thread_count,
    std::size_t dynamic_shared_memory_bytes) {
    warp_specialized_pipeline_kernel<
        TasksPerBatch, SharedQueueDepth>
        <<<1, thread_count, dynamic_shared_memory_bytes>>>(
            control, task_slots, completion_slots,
            capacity_mask, copy_warps, stage_buffer_base);
}

template <std::uint32_t SharedQueueDepth>
bool dispatch_warp_specialized_pipeline_batch(
    std::uint32_t device_batch,
    WarpSpecializedControl *control,
    WarpSpecializedTaskSlot *task_slots,
    WarpSpecializedCompletionSlot *completion_slots,
    std::uint64_t capacity_mask, std::uint32_t copy_warps,
    std::uint64_t stage_buffer_base,
    std::uint32_t thread_count,
    std::size_t dynamic_shared_memory_bytes) {
#define UGDR_LAUNCH_PIPELINE_BATCH(batch)                       \
    launch_warp_specialized_pipeline_kernel<                    \
        batch, SharedQueueDepth>(                               \
        control, task_slots, completion_slots, capacity_mask,   \
        copy_warps, stage_buffer_base, thread_count,            \
        dynamic_shared_memory_bytes)
    switch (device_batch) {
    case 1:
        UGDR_LAUNCH_PIPELINE_BATCH(1);
        return true;
    case 2:
        if constexpr (SharedQueueDepth >= 2) {
            UGDR_LAUNCH_PIPELINE_BATCH(2);
            return true;
        }
        return false;
    case 4:
        if constexpr (SharedQueueDepth >= 4) {
            UGDR_LAUNCH_PIPELINE_BATCH(4);
            return true;
        }
        return false;
    case 8:
        if constexpr (SharedQueueDepth >= 8) {
            UGDR_LAUNCH_PIPELINE_BATCH(8);
            return true;
        }
        return false;
    case 16:
        if constexpr (SharedQueueDepth >= 16) {
            UGDR_LAUNCH_PIPELINE_BATCH(16);
            return true;
        }
        return false;
    case 32:
        if constexpr (SharedQueueDepth >= 32) {
            UGDR_LAUNCH_PIPELINE_BATCH(32);
            return true;
        }
        return false;
    default:
        return false;
    }
#undef UGDR_LAUNCH_PIPELINE_BATCH
}

template <std::uint32_t SharedQueueDepth>
bool dispatch_warp_specialized_variant(
    bool use_pipeline, std::uint32_t device_batch,
    WarpSpecializedControl *control,
    WarpSpecializedTaskSlot *task_slots,
    WarpSpecializedCompletionSlot *completion_slots,
    std::uint64_t capacity_mask, std::uint32_t copy_warps,
    std::uint64_t stage_buffer_base,
    std::uint32_t thread_count,
    std::size_t dynamic_shared_memory_bytes) {
    if (use_pipeline) {
        return dispatch_warp_specialized_pipeline_batch<
            SharedQueueDepth>(
            device_batch, control, task_slots, completion_slots,
            capacity_mask, copy_warps, stage_buffer_base,
            thread_count, dynamic_shared_memory_bytes);
    }
    return dispatch_warp_specialized_batch<SharedQueueDepth>(
        device_batch, control, task_slots, completion_slots,
        capacity_mask, copy_warps, stage_buffer_base,
        thread_count, dynamic_shared_memory_bytes);
}

int cuda_status(cudaError_t status) noexcept {
    switch (status) {
    case cudaSuccess:
        return 0;
    case cudaErrorInvalidValue:
    case cudaErrorInvalidDevice:
    case cudaErrorInvalidResourceHandle:
        return EINVAL;
    case cudaErrorMemoryAllocation:
        return ENOMEM;
    case cudaErrorNoDevice:
    case cudaErrorInsufficientDriver:
        return ENODEV;
    case cudaErrorNotSupported:
        return EOPNOTSUPP;
    default:
        return EIO;
    }
}

int driver_status(CUresult status) noexcept {
    switch (status) {
    case CUDA_SUCCESS:
        return 0;
    case CUDA_ERROR_INVALID_VALUE:
    case CUDA_ERROR_INVALID_DEVICE:
    case CUDA_ERROR_INVALID_HANDLE:
        return EINVAL;
    case CUDA_ERROR_OUT_OF_MEMORY:
        return ENOMEM;
    case CUDA_ERROR_NO_DEVICE:
    case CUDA_ERROR_SYSTEM_DRIVER_MISMATCH:
        return ENODEV;
    case CUDA_ERROR_NOT_SUPPORTED:
        return EOPNOTSUPP;
    default:
        return EIO;
    }
}

bool checked_allocation_size(std::size_t payload_capacity, std::size_t guard_bytes,
                             std::size_t *allocation_size) noexcept {
    if (payload_capacity == 0 || guard_bytes == 0 ||
        guard_bytes > (std::numeric_limits<std::size_t>::max() - payload_capacity) / 2) {
        return false;
    }
    *allocation_size = payload_capacity + guard_bytes * 2;
    return true;
}

std::uint64_t pointer_address(const void *pointer) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

bool is_power_of_two(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

bool is_supported_device_batch(std::uint32_t value) noexcept {
    return value == 1 || value == 2 || value == 4 || value == 8 || value == 16 || value == 32;
}

bool is_supported_shared_queue_depth(
    std::uint32_t value) noexcept {
    return value == 2 || value == 4 || value == 8 ||
           value == 16 || value == 32 || value == 64 ||
           value == 128 || value == 256 || value == 512;
}

bool direct_atomic_allocation_size(std::size_t capacity, std::size_t *bytes) noexcept {
    constexpr std::size_t kPerSlotBytes =
        sizeof(DirectAtomicTaskSlot) + sizeof(DirectAtomicCompletionSlot);
    if (!is_power_of_two(capacity) ||
        capacity > (std::numeric_limits<std::size_t>::max() - sizeof(DirectAtomicControl)) /
                       kPerSlotBytes) {
        return false;
    }
    *bytes = sizeof(DirectAtomicControl) + capacity * kPerSlotBytes;
    return true;
}

bool dynamic_sharded_spsc_allocation_size(std::size_t capacity,
                                          std::uint32_t lane_count,
                                          std::size_t *bytes) noexcept {
    constexpr std::size_t kPerSlotBytes =
        sizeof(DynamicShardedSpscTaskSlot) +
        sizeof(DynamicShardedSpscCompletionSlot);
    if (lane_count == 0 || lane_count > 32 || !is_power_of_two(capacity) ||
        capacity < lane_count || capacity % lane_count != 0 ||
        !is_power_of_two(capacity / lane_count)) {
        return false;
    }
    const std::size_t control_bytes =
        static_cast<std::size_t>(lane_count) *
        sizeof(DynamicShardedSpscControl);
    if (capacity >
        (std::numeric_limits<std::size_t>::max() - control_bytes) /
            kPerSlotBytes) {
        return false;
    }
    *bytes = control_bytes + capacity * kPerSlotBytes;
    return true;
}

bool static_partition_spsc_allocation_size(std::size_t capacity,
                                           std::uint32_t copy_warps,
                                           std::size_t *bytes) noexcept {
    constexpr std::size_t kPerSlotBytes =
        sizeof(StaticPartitionSpscTaskSlot) +
        sizeof(StaticPartitionSpscCompletionSlot);
    if (copy_warps == 0 || copy_warps > 32 ||
        !is_power_of_two(copy_warps) || !is_power_of_two(capacity) ||
        capacity < copy_warps ||
        (capacity & (static_cast<std::size_t>(copy_warps) - 1)) != 0) {
        return false;
    }
    if (capacity >
        (std::numeric_limits<std::size_t>::max() -
         sizeof(StaticPartitionSpscControl)) /
            kPerSlotBytes) {
        return false;
    }
    *bytes = sizeof(StaticPartitionSpscControl) +
             capacity * kPerSlotBytes;
    return true;
}

bool warp_specialized_allocation_size(
    std::size_t capacity, std::uint32_t copy_warps,
    std::uint32_t device_batch, std::uint32_t shared_queue_depth,
    std::size_t *host_bytes, std::size_t *shared_bytes) noexcept {
    constexpr std::size_t kPerHostSlotBytes =
        sizeof(WarpSpecializedTaskSlot) +
        sizeof(WarpSpecializedCompletionSlot);
    constexpr std::size_t kPerSharedSlotBytes =
        sizeof(WarpSpecializedSharedTaskSlot) +
        sizeof(WarpSpecializedSharedCompletionSlot);
    if (copy_warps == 0 || copy_warps > 30 ||
        !is_supported_device_batch(device_batch) ||
        !is_power_of_two(capacity) ||
        !is_supported_shared_queue_depth(
            shared_queue_depth) ||
        device_batch > shared_queue_depth ||
        capacity >
            (std::numeric_limits<std::size_t>::max() -
             sizeof(WarpSpecializedControl)) /
                kPerHostSlotBytes ||
        shared_queue_depth >
            (std::numeric_limits<std::size_t>::max() -
             sizeof(WarpSpecializedSharedControl)) /
                kPerSharedSlotBytes) {
        return false;
    }
    *host_bytes = sizeof(WarpSpecializedControl) +
                  capacity * kPerHostSlotBytes;
    *shared_bytes = sizeof(WarpSpecializedSharedControl) +
                    static_cast<std::size_t>(shared_queue_depth) *
                        kPerSharedSlotBytes;
    return true;
}

bool warp_specialized_pipeline_allocation_size(
    std::size_t capacity, std::uint32_t copy_warps,
    std::uint32_t device_batch,
    std::uint32_t shared_queue_depth,
    std::size_t *host_bytes,
    std::size_t *shared_bytes) noexcept {
    std::size_t unused_shared_bytes = 0;
    if (shared_queue_depth > 16 ||
        !warp_specialized_allocation_size(
            capacity, copy_warps, device_batch,
            shared_queue_depth, host_bytes,
            &unused_shared_bytes)) {
        return false;
    }
    constexpr std::size_t kPerSharedSlotBytes =
        sizeof(WarpSpecializedPipelineTaskSlot) +
        sizeof(WarpSpecializedPipelineCompletionSlot);
    const std::size_t slot_count =
        static_cast<std::size_t>(copy_warps) *
        shared_queue_depth;
    if (slot_count >
        (std::numeric_limits<std::size_t>::max() - 64) /
            kPerSharedSlotBytes) {
        return false;
    }
    *shared_bytes = 64 + slot_count * kPerSharedSlotBytes;
    return *shared_bytes <= 48 * 1024;
}

DirectAtomicControl *direct_atomic_control(void *memory) noexcept {
    return static_cast<DirectAtomicControl *>(memory);
}

DirectAtomicTaskSlot *direct_atomic_task_slots(void *memory) noexcept {
    return reinterpret_cast<DirectAtomicTaskSlot *>(direct_atomic_control(memory) + 1);
}

DirectAtomicCompletionSlot *direct_atomic_completion_slots(void *memory,
                                                           std::size_t capacity) noexcept {
    return reinterpret_cast<DirectAtomicCompletionSlot *>(direct_atomic_task_slots(memory) +
                                                          capacity);
}

DynamicShardedSpscControl *dynamic_sharded_spsc_controls(void *memory) noexcept {
    return static_cast<DynamicShardedSpscControl *>(memory);
}

DynamicShardedSpscTaskSlot *dynamic_sharded_spsc_task_slots(
    void *memory, std::uint32_t lane_count) noexcept {
    return reinterpret_cast<DynamicShardedSpscTaskSlot *>(
        dynamic_sharded_spsc_controls(memory) + lane_count);
}

DynamicShardedSpscCompletionSlot *dynamic_sharded_spsc_completion_slots(
    void *memory, std::uint32_t lane_count, std::size_t capacity) noexcept {
    return reinterpret_cast<DynamicShardedSpscCompletionSlot *>(
        dynamic_sharded_spsc_task_slots(memory, lane_count) + capacity);
}

StaticPartitionSpscControl *static_partition_spsc_control(
    void *memory) noexcept {
    return static_cast<StaticPartitionSpscControl *>(memory);
}

StaticPartitionSpscTaskSlot *static_partition_spsc_task_slots(
    void *memory) noexcept {
    return reinterpret_cast<StaticPartitionSpscTaskSlot *>(
        static_partition_spsc_control(memory) + 1);
}

StaticPartitionSpscCompletionSlot *static_partition_spsc_completion_slots(
    void *memory, std::size_t capacity) noexcept {
    return reinterpret_cast<StaticPartitionSpscCompletionSlot *>(
        static_partition_spsc_task_slots(memory) + capacity);
}

const StaticPartitionSpscCompletionSlot *
static_partition_spsc_completion_slots(const void *memory,
                                       std::size_t capacity) noexcept {
    const auto *const control =
        static_cast<const StaticPartitionSpscControl *>(memory);
    const auto *const task_slots =
        reinterpret_cast<const StaticPartitionSpscTaskSlot *>(control + 1);
    return reinterpret_cast<const StaticPartitionSpscCompletionSlot *>(
        task_slots + capacity);
}

WarpSpecializedControl *warp_specialized_control(
    void *memory) noexcept {
    return static_cast<WarpSpecializedControl *>(memory);
}

WarpSpecializedTaskSlot *warp_specialized_task_slots(
    void *memory) noexcept {
    return reinterpret_cast<WarpSpecializedTaskSlot *>(
        warp_specialized_control(memory) + 1);
}

WarpSpecializedCompletionSlot *warp_specialized_completion_slots(
    void *memory, std::size_t capacity) noexcept {
    return reinterpret_cast<WarpSpecializedCompletionSlot *>(
        warp_specialized_task_slots(memory) + capacity);
}

std::uint64_t host_load_acquire(const std::uint64_t *address) noexcept {
    return __atomic_load_n(address, __ATOMIC_ACQUIRE);
}

void host_store_release(std::uint64_t *address, std::uint64_t value) noexcept {
    __atomic_store_n(address, value, __ATOMIC_RELEASE);
}

}  // namespace

const char *persistent_copy_model_name(PersistentCopyModel model) noexcept {
    switch (model) {
    case PersistentCopyModel::direct_atomic:
        return "direct_atomic";
    case PersistentCopyModel::dynamic_sharded_spsc:
        return "dynamic_sharded_spsc";
    case PersistentCopyModel::static_partition_spsc:
        return "static_partition_spsc";
    case PersistentCopyModel::warp_specialized:
        return "warp_specialized";
    case PersistentCopyModel::warp_specialized_pipeline:
        return "warp_specialized_pipeline";
    }
    return "unknown";
}

int parse_persistent_copy_model(const char *name, PersistentCopyModel *model) noexcept {
    if (name == nullptr || model == nullptr) {
        return EINVAL;
    }
    constexpr PersistentCopyModel models[]{
        PersistentCopyModel::direct_atomic,
        PersistentCopyModel::dynamic_sharded_spsc,
        PersistentCopyModel::static_partition_spsc,
        PersistentCopyModel::warp_specialized,
        PersistentCopyModel::warp_specialized_pipeline,
    };
    for (const PersistentCopyModel candidate : models) {
        if (std::strcmp(name, persistent_copy_model_name(candidate)) == 0) {
            *model = candidate;
            return 0;
        }
    }
    return EINVAL;
}

int validate_persistent_copy_config(const PersistentCopyConfig &config) noexcept {
    switch (config.model) {
    case PersistentCopyModel::direct_atomic:
    case PersistentCopyModel::dynamic_sharded_spsc:
    case PersistentCopyModel::static_partition_spsc:
    case PersistentCopyModel::warp_specialized:
    case PersistentCopyModel::warp_specialized_pipeline:
        break;
    default:
        return EINVAL;
    }
    if (config.payload_bytes == 0 || config.payload_bytes > kPersistentCopyMaxPayloadBytes ||
        config.parent_wr_bytes == 0 || !is_power_of_two(config.outstanding_capacity) ||
        config.host_batch == 0 ||
        config.host_batch > config.outstanding_capacity ||
        !is_supported_device_batch(config.device_batch) ||
        config.device_batch > config.outstanding_capacity || config.copy_warps == 0 ||
        config.copy_warps > 32 || config.warmup_tasks == 0 || config.iterations == 0) {
        return EINVAL;
    }
    if (config.model == PersistentCopyModel::dynamic_sharded_spsc) {
        std::size_t bytes = 0;
        if (!dynamic_sharded_spsc_allocation_size(
                config.outstanding_capacity, config.copy_warps, &bytes)) {
            return EINVAL;
        }
    }
    if (config.model == PersistentCopyModel::static_partition_spsc) {
        std::size_t bytes = 0;
        if (!static_partition_spsc_allocation_size(
                config.outstanding_capacity, config.copy_warps, &bytes)) {
            return EINVAL;
        }
    }
    if (config.model == PersistentCopyModel::warp_specialized ||
        config.model ==
            PersistentCopyModel::warp_specialized_pipeline) {
        std::size_t host_bytes = 0;
        std::size_t shared_bytes = 0;
        const bool valid =
            config.model ==
                    PersistentCopyModel::
                        warp_specialized_pipeline
                ? warp_specialized_pipeline_allocation_size(
                      config.outstanding_capacity,
                      config.copy_warps,
                      config.device_batch,
                      config.shared_queue_depth,
                      &host_bytes, &shared_bytes)
                : warp_specialized_allocation_size(
                      config.outstanding_capacity,
                      config.copy_warps,
                      config.device_batch,
                      config.shared_queue_depth,
                      &host_bytes, &shared_bytes);
        if (!valid) {
            return EINVAL;
        }
    }
    return 0;
}

int initialize_persistent_copy_device(std::uint32_t device_ordinal) noexcept {
    int device_count = 0;
    cudaError_t status = cudaGetDeviceCount(&device_count);
    if (status != cudaSuccess) {
        (void)cudaGetLastError();
        return cuda_status(status);
    }
    if (device_ordinal >= static_cast<std::uint32_t>(device_count)) {
        return EINVAL;
    }
    status = cudaSetDevice(static_cast<int>(device_ordinal));
    if (status != cudaSuccess) {
        return cuda_status(status);
    }
    cudaDeviceProp properties{};
    status = cudaGetDeviceProperties(&properties, static_cast<int>(device_ordinal));
    if (status != cudaSuccess) {
        return cuda_status(status);
    }
    if (properties.canMapHostMemory == 0) {
        return EOPNOTSUPP;
    }
    return cuda_status(cudaFree(nullptr));
}

int launch_persistent_copy_core_test(std::uint64_t stage_buffer_base, const CopyTask &task,
                                     std::uint64_t access_counts_address) noexcept {
    if (stage_buffer_base == 0 || task.target_address == 0 || task.length == 0 ||
        access_counts_address == 0) {
        return EINVAL;
    }
    persistent_copy_core_test_kernel<<<1, 32>>>(stage_buffer_base, task, access_counts_address);
    return cuda_status(cudaGetLastError());
}

MappedPinnedMemory::~MappedPinnedMemory() {
    (void)reset();
}

MappedPinnedMemory::MappedPinnedMemory(MappedPinnedMemory &&other) noexcept
    : host_data_(std::exchange(other.host_data_, nullptr)),
      device_data_(std::exchange(other.device_data_, nullptr)),
      size_(std::exchange(other.size_, 0)) {
}

MappedPinnedMemory &MappedPinnedMemory::operator=(MappedPinnedMemory &&other) noexcept {
    if (this != &other) {
        (void)reset();
        host_data_ = std::exchange(other.host_data_, nullptr);
        device_data_ = std::exchange(other.device_data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

int MappedPinnedMemory::allocate(std::size_t bytes, MappedPinnedMemory *memory) noexcept {
    if (bytes == 0 || memory == nullptr || !memory->empty()) {
        return EINVAL;
    }
    void *host_data = nullptr;
    const cudaError_t allocation_status =
        cudaHostAlloc(&host_data, bytes, cudaHostAllocMapped | cudaHostAllocPortable);
    if (allocation_status != cudaSuccess) {
        return cuda_status(allocation_status);
    }
    void *device_data = nullptr;
    const cudaError_t mapping_status = cudaHostGetDevicePointer(&device_data, host_data, 0);
    if (mapping_status != cudaSuccess) {
        (void)cudaFreeHost(host_data);
        return cuda_status(mapping_status);
    }
    memory->host_data_ = host_data;
    memory->device_data_ = device_data;
    memory->size_ = bytes;
    return 0;
}

int MappedPinnedMemory::reset() noexcept {
    if (host_data_ == nullptr) {
        device_data_ = nullptr;
        size_ = 0;
        return 0;
    }
    void *const host_data = std::exchange(host_data_, nullptr);
    device_data_ = nullptr;
    size_ = 0;
    return cuda_status(cudaFreeHost(host_data));
}

void *MappedPinnedMemory::host_data() noexcept {
    return host_data_;
}

const void *MappedPinnedMemory::host_data() const noexcept {
    return host_data_;
}

std::uint64_t MappedPinnedMemory::device_address() const noexcept {
    return pointer_address(device_data_);
}

std::size_t MappedPinnedMemory::size() const noexcept {
    return size_;
}

bool MappedPinnedMemory::empty() const noexcept {
    return host_data_ == nullptr;
}

std::uint8_t persistent_copy_payload_byte(std::uint64_t seed, std::size_t index) noexcept {
    std::uint64_t value = seed ^ (static_cast<std::uint64_t>(index) + UINT64_C(0x9e3779b97f4a7c15));
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return static_cast<std::uint8_t>(value);
}

PersistentCopyPayloadBuffer::~PersistentCopyPayloadBuffer() {
    (void)reset();
}

PersistentCopyPayloadBuffer::PersistentCopyPayloadBuffer(
    PersistentCopyPayloadBuffer &&other) noexcept
    : stage_buffer_allocation_(std::exchange(other.stage_buffer_allocation_, nullptr)),
      target_allocation_(std::exchange(other.target_allocation_, nullptr)),
      payload_capacity_(std::exchange(other.payload_capacity_, 0)),
      guard_bytes_(std::exchange(other.guard_bytes_, 0)) {
}

PersistentCopyPayloadBuffer &
PersistentCopyPayloadBuffer::operator=(PersistentCopyPayloadBuffer &&other) noexcept {
    if (this != &other) {
        (void)reset();
        stage_buffer_allocation_ = std::exchange(other.stage_buffer_allocation_, nullptr);
        target_allocation_ = std::exchange(other.target_allocation_, nullptr);
        payload_capacity_ = std::exchange(other.payload_capacity_, 0);
        guard_bytes_ = std::exchange(other.guard_bytes_, 0);
    }
    return *this;
}

int PersistentCopyPayloadBuffer::allocate(std::size_t payload_capacity, std::size_t guard_bytes,
                                          PersistentCopyPayloadBuffer *buffer) noexcept {
    std::size_t allocation_size = 0;
    if (buffer == nullptr || !buffer->empty() ||
        payload_capacity > kPersistentCopyMaxStageBufferBytes ||
        !checked_allocation_size(payload_capacity, guard_bytes, &allocation_size)) {
        return EINVAL;
    }
    void *stage_buffer = nullptr;
    cudaError_t status = cudaMalloc(&stage_buffer, allocation_size);
    if (status != cudaSuccess) {
        return cuda_status(status);
    }
    void *target = nullptr;
    status = cudaMalloc(&target, allocation_size);
    if (status != cudaSuccess) {
        (void)cudaFree(stage_buffer);
        return cuda_status(status);
    }
    buffer->stage_buffer_allocation_ = stage_buffer;
    buffer->target_allocation_ = target;
    buffer->payload_capacity_ = payload_capacity;
    buffer->guard_bytes_ = guard_bytes;
    return 0;
}

int PersistentCopyPayloadBuffer::prepare(std::uint64_t seed) noexcept {
    std::size_t allocation_size = 0;
    if (empty() || !checked_allocation_size(payload_capacity_, guard_bytes_, &allocation_size)) {
        return EINVAL;
    }
    try {
        std::vector<std::uint8_t> stage_buffer(allocation_size, kPayloadGuardByte);
        std::vector<std::uint8_t> target(allocation_size, kPayloadGuardByte);
        for (std::size_t index = 0; index < payload_capacity_; ++index) {
            stage_buffer[guard_bytes_ + index] = persistent_copy_payload_byte(seed, index);
            target[guard_bytes_ + index] = kPayloadInitialTargetByte;
        }
        CUresult status =
            cuMemcpyHtoD(static_cast<CUdeviceptr>(pointer_address(stage_buffer_allocation_)),
                         stage_buffer.data(), stage_buffer.size());
        if (status != CUDA_SUCCESS) {
            return driver_status(status);
        }
        status = cuMemcpyHtoD(static_cast<CUdeviceptr>(pointer_address(target_allocation_)),
                              target.data(), target.size());
        return driver_status(status);
    } catch (const std::bad_alloc &) {
        return ENOMEM;
    } catch (...) {
        return EIO;
    }
}

int PersistentCopyPayloadBuffer::verify(std::uint64_t seed, PayloadCheck *check) const noexcept {
    return verify_copy(seed, 0, 0, payload_capacity_, check);
}

int PersistentCopyPayloadBuffer::verify_copy(std::uint64_t seed, std::size_t source_offset,
                                             std::size_t target_offset, std::size_t length,
                                             PayloadCheck *check) const noexcept {
    std::size_t allocation_size = 0;
    if (check == nullptr || empty() ||
        source_offset > payload_capacity_ || length > payload_capacity_ - source_offset ||
        target_offset > payload_capacity_ || length > payload_capacity_ - target_offset ||
        !checked_allocation_size(payload_capacity_, guard_bytes_, &allocation_size)) {
        return EINVAL;
    }
    try {
        std::vector<std::uint8_t> observed(allocation_size);
        const CUresult status = cuMemcpyDtoH(
            observed.data(), static_cast<CUdeviceptr>(pointer_address(target_allocation_)),
            observed.size());
        if (status != CUDA_SUCCESS) {
            return driver_status(status);
        }
        PayloadCheck result;
        result.guards_intact = true;
        result.first_mismatch = payload_capacity_;
        for (std::size_t index = 0; index < guard_bytes_; ++index) {
            if (observed[index] != kPayloadGuardByte ||
                observed[guard_bytes_ + payload_capacity_ + index] != kPayloadGuardByte) {
                result.guards_intact = false;
            }
        }
        for (std::size_t index = 0; index < payload_capacity_; ++index) {
            std::uint8_t expected = kPayloadInitialTargetByte;
            if (index >= target_offset && index - target_offset < length) {
                expected = persistent_copy_payload_byte(seed, source_offset + index - target_offset);
            }
            if (observed[guard_bytes_ + index] != expected) {
                if (result.mismatch_count == 0) {
                    result.first_mismatch = index;
                }
                ++result.mismatch_count;
            }
        }
        result.payload_matches = result.mismatch_count == 0;
        *check = result;
        return 0;
    } catch (const std::bad_alloc &) {
        return ENOMEM;
    } catch (...) {
        return EIO;
    }
}

int PersistentCopyPayloadBuffer::make_task(std::uint64_t task_id, std::uint64_t target_address,
                                           std::size_t length, std::uint32_t relative_offset,
                                           CopyTask *task) const noexcept {
    const std::size_t offset = relative_offset;
    if (task == nullptr || empty() || target_address == 0 || length == 0 ||
        offset > payload_capacity_ || length > payload_capacity_ - offset ||
        length > std::numeric_limits<std::uint32_t>::max()) {
        return EINVAL;
    }
    CopyTask result;
    result.task_id = task_id;
    result.target_address = target_address;
    result.length = static_cast<std::uint32_t>(length);
    result.relative_offset = relative_offset;
    *task = result;
    return 0;
}

int PersistentCopyPayloadBuffer::reset() noexcept {
    void *const stage_buffer = std::exchange(stage_buffer_allocation_, nullptr);
    void *const target = std::exchange(target_allocation_, nullptr);
    payload_capacity_ = 0;
    guard_bytes_ = 0;
    int status = 0;
    if (stage_buffer != nullptr) {
        status = cuda_status(cudaFree(stage_buffer));
    }
    if (target != nullptr) {
        const int target_status = cuda_status(cudaFree(target));
        if (status == 0) {
            status = target_status;
        }
    }
    return status;
}

std::uint64_t PersistentCopyPayloadBuffer::stage_buffer_base() const noexcept {
    if (stage_buffer_allocation_ == nullptr) {
        return 0;
    }
    return pointer_address(static_cast<const std::uint8_t *>(stage_buffer_allocation_) +
                           guard_bytes_);
}

std::uint64_t PersistentCopyPayloadBuffer::target_address() const noexcept {
    if (target_allocation_ == nullptr) {
        return 0;
    }
    return pointer_address(static_cast<const std::uint8_t *>(target_allocation_) + guard_bytes_);
}

std::size_t PersistentCopyPayloadBuffer::payload_capacity() const noexcept {
    return payload_capacity_;
}

std::size_t PersistentCopyPayloadBuffer::guard_bytes() const noexcept {
    return guard_bytes_;
}

bool PersistentCopyPayloadBuffer::empty() const noexcept {
    return stage_buffer_allocation_ == nullptr && target_allocation_ == nullptr;
}

DirectAtomicQueue::~DirectAtomicQueue() {
    if (running_) {
        (void)request_stop();
        (void)wait();
    }
    (void)reset();
}

int DirectAtomicQueue::allocate(std::size_t capacity, std::uint32_t copy_warps,
                                std::uint32_t device_batch, std::uint64_t stage_buffer_base,
                                DirectAtomicQueue *queue) noexcept {
    std::size_t bytes = 0;
    if (queue == nullptr || !queue->empty() || copy_warps == 0 || copy_warps > 32 ||
        !is_supported_device_batch(device_batch) || device_batch > capacity ||
        stage_buffer_base == 0 || !direct_atomic_allocation_size(capacity, &bytes)) {
        return EINVAL;
    }
    const int status = MappedPinnedMemory::allocate(bytes, &queue->memory_);
    if (status != 0) {
        return status;
    }
    queue->capacity_ = capacity;
    queue->capacity_mask_ = capacity - 1;
    queue->copy_warps_ = copy_warps;
    queue->device_batch_ = device_batch;
    queue->stage_buffer_base_ = stage_buffer_base;
    return 0;
}

int DirectAtomicQueue::start() noexcept {
    if (empty() || running_) {
        return EINVAL;
    }
    std::memset(memory_.host_data(), 0, memory_.size());
    submit_tail_ = 0;
    completion_head_ = 0;
    completed_device_batches_ = 0;
    accepting_ = true;
    running_ = true;

    const std::uintptr_t device_base =
        static_cast<std::uintptr_t>(memory_.device_address());
    auto *const control_device =
        reinterpret_cast<DirectAtomicControl *>(device_base);
    auto *const task_slots_device = reinterpret_cast<DirectAtomicTaskSlot *>(
        device_base + sizeof(DirectAtomicControl));
    auto *const completion_slots_device = reinterpret_cast<DirectAtomicCompletionSlot *>(
        device_base + sizeof(DirectAtomicControl) +
        capacity_ * sizeof(DirectAtomicTaskSlot));
    switch (device_batch_) {
    case 1:
        direct_atomic_kernel<1><<<1, copy_warps_ * 32>>>(
            control_device, task_slots_device, completion_slots_device, capacity_mask_,
            stage_buffer_base_);
        break;
    case 2:
        direct_atomic_kernel<2><<<1, copy_warps_ * 32>>>(
            control_device, task_slots_device, completion_slots_device, capacity_mask_,
            stage_buffer_base_);
        break;
    case 4:
        direct_atomic_kernel<4><<<1, copy_warps_ * 32>>>(
            control_device, task_slots_device, completion_slots_device, capacity_mask_,
            stage_buffer_base_);
        break;
    case 8:
        direct_atomic_kernel<8><<<1, copy_warps_ * 32>>>(
            control_device, task_slots_device, completion_slots_device, capacity_mask_,
            stage_buffer_base_);
        break;
    case 16:
        direct_atomic_kernel<16><<<1, copy_warps_ * 32>>>(
            control_device, task_slots_device, completion_slots_device, capacity_mask_,
            stage_buffer_base_);
        break;
    case 32:
        direct_atomic_kernel<32><<<1, copy_warps_ * 32>>>(
            control_device, task_slots_device, completion_slots_device, capacity_mask_,
            stage_buffer_base_);
        break;
    default:
        running_ = false;
        accepting_ = false;
        return EINVAL;
    }
    const int status = cuda_status(cudaGetLastError());
    if (status != 0) {
        running_ = false;
        accepting_ = false;
    }
    return status;
}

int DirectAtomicQueue::try_submit(const CopyTask &task) noexcept {
    std::size_t submitted_count = 0;
    return try_submit_batch(&task, 1, &submitted_count);
}

int DirectAtomicQueue::try_submit_batch(const CopyTask *tasks, std::size_t task_count,
                                        std::size_t *submitted_count) noexcept {
    if (submitted_count != nullptr) {
        *submitted_count = 0;
    }
    if (!running_ || !accepting_ || tasks == nullptr || task_count == 0 ||
        submitted_count == nullptr) {
        return EINVAL;
    }
    const std::size_t outstanding =
        static_cast<std::size_t>(submit_tail_ - completion_head_);
    if (outstanding >= capacity_) {
        return EAGAIN;
    }
    const std::size_t available = capacity_ - outstanding;
    const std::size_t count = task_count < available ? task_count : available;
    for (std::size_t index = 0; index < count; ++index) {
        if (tasks[index].target_address == 0 || tasks[index].length == 0) {
            return EINVAL;
        }
    }

    auto *const control = direct_atomic_control(memory_.host_data());
    auto *const slots = direct_atomic_task_slots(memory_.host_data());
    for (std::size_t index = 0; index < count; ++index) {
        slots[(submit_tail_ + index) & capacity_mask_].task = tasks[index];
    }
    submit_tail_ += count;
    host_store_release(&control->task_tail, submit_tail_);
    *submitted_count = count;
    return 0;
}

int DirectAtomicQueue::try_poll(CopyCompletion *completion) noexcept {
    if (completion == nullptr || empty()) {
        return EINVAL;
    }
    auto *const slot = &direct_atomic_completion_slots(memory_.host_data(), capacity_)
                            [completion_head_ & capacity_mask_];
    if (host_load_acquire(&slot->sequence) != completion_head_ + 1) {
        return EAGAIN;
    }
    *completion = slot->completion;
    completed_device_batches_ += slot->batch_start;
    ++completion_head_;
    return 0;
}

int DirectAtomicQueue::request_stop() noexcept {
    if (!running_ || !accepting_) {
        return EINVAL;
    }
    accepting_ = false;
    auto *const control = direct_atomic_control(memory_.host_data());
    host_store_release(&control->stop_requested, 1);
    return 0;
}

int DirectAtomicQueue::wait() noexcept {
    if (!running_ || accepting_) {
        return EINVAL;
    }
    const int status = cuda_status(cudaDeviceSynchronize());
    running_ = false;
    return status;
}

int DirectAtomicQueue::reset() noexcept {
    if (running_) {
        return EBUSY;
    }
    const int status = memory_.reset();
    capacity_ = 0;
    capacity_mask_ = 0;
    copy_warps_ = 0;
    device_batch_ = 0;
    stage_buffer_base_ = 0;
    submit_tail_ = 0;
    completion_head_ = 0;
    completed_device_batches_ = 0;
    accepting_ = false;
    return status;
}

std::size_t DirectAtomicQueue::capacity() const noexcept {
    return capacity_;
}

std::size_t DirectAtomicQueue::host_meta_bytes() const noexcept {
    return memory_.size();
}

std::uint32_t DirectAtomicQueue::copy_warps() const noexcept {
    return copy_warps_;
}

std::uint32_t DirectAtomicQueue::device_batch() const noexcept {
    return device_batch_;
}

std::uint64_t DirectAtomicQueue::accepted_tasks() const noexcept {
    return submit_tail_;
}

std::uint64_t DirectAtomicQueue::completed_tasks() const noexcept {
    return completion_head_;
}

std::uint64_t DirectAtomicQueue::host_system_atomic_operations() const noexcept {
    return completed_device_batches_ * 2;
}

bool DirectAtomicQueue::running() const noexcept {
    return running_;
}

bool DirectAtomicQueue::accepting() const noexcept {
    return accepting_;
}

bool DirectAtomicQueue::drained() const noexcept {
    return submit_tail_ == completion_head_;
}

bool DirectAtomicQueue::empty() const noexcept {
    return memory_.empty();
}

DynamicShardedSpscQueue::~DynamicShardedSpscQueue() {
    if (running_) {
        (void)request_stop();
        (void)wait();
    }
    (void)reset();
}

int DynamicShardedSpscQueue::allocate(
    std::size_t capacity, std::uint32_t copy_warps,
    std::uint32_t device_batch, std::uint64_t stage_buffer_base,
    DynamicShardedSpscQueue *queue) noexcept {
    std::size_t bytes = 0;
    if (queue == nullptr || !queue->empty() ||
        !is_supported_device_batch(device_batch) ||
        device_batch > capacity || stage_buffer_base == 0 ||
        !dynamic_sharded_spsc_allocation_size(capacity, copy_warps, &bytes)) {
        return EINVAL;
    }
    const int status = MappedPinnedMemory::allocate(bytes, &queue->memory_);
    if (status != 0) {
        return status;
    }
    queue->capacity_ = capacity;
    queue->lane_capacity_ = capacity / copy_warps;
    queue->lane_capacity_mask_ = queue->lane_capacity_ - 1;
    queue->lane_count_ = copy_warps;
    queue->device_batch_ = device_batch;
    queue->stage_buffer_base_ = stage_buffer_base;
    return 0;
}

int DynamicShardedSpscQueue::start() noexcept {
    if (empty() || running_) {
        return EINVAL;
    }
    std::memset(memory_.host_data(), 0, memory_.size());
    std::memset(submit_tails_, 0, sizeof(submit_tails_));
    std::memset(completion_heads_, 0, sizeof(completion_heads_));
    submit_cursor_ = 0;
    completion_cursor_ = 0;
    accepted_tasks_ = 0;
    completed_tasks_ = 0;
    accepting_ = true;
    running_ = true;

    const std::uintptr_t device_base =
        static_cast<std::uintptr_t>(memory_.device_address());
    auto *const controls_device =
        reinterpret_cast<DynamicShardedSpscControl *>(device_base);
    auto *const task_slots_device =
        reinterpret_cast<DynamicShardedSpscTaskSlot *>(
            device_base + lane_count_ *
                              sizeof(DynamicShardedSpscControl));
    auto *const completion_slots_device =
        reinterpret_cast<DynamicShardedSpscCompletionSlot *>(
            device_base + lane_count_ *
                              sizeof(DynamicShardedSpscControl) +
            capacity_ * sizeof(DynamicShardedSpscTaskSlot));
    switch (device_batch_) {
    case 1:
        dynamic_sharded_spsc_kernel<1><<<1, lane_count_ * 32>>>(
            controls_device, task_slots_device, completion_slots_device,
            lane_capacity_, lane_capacity_mask_, stage_buffer_base_);
        break;
    case 2:
        dynamic_sharded_spsc_kernel<2><<<1, lane_count_ * 32>>>(
            controls_device, task_slots_device, completion_slots_device,
            lane_capacity_, lane_capacity_mask_, stage_buffer_base_);
        break;
    case 4:
        dynamic_sharded_spsc_kernel<4><<<1, lane_count_ * 32>>>(
            controls_device, task_slots_device, completion_slots_device,
            lane_capacity_, lane_capacity_mask_, stage_buffer_base_);
        break;
    case 8:
        dynamic_sharded_spsc_kernel<8><<<1, lane_count_ * 32>>>(
            controls_device, task_slots_device, completion_slots_device,
            lane_capacity_, lane_capacity_mask_, stage_buffer_base_);
        break;
    case 16:
        dynamic_sharded_spsc_kernel<16><<<1, lane_count_ * 32>>>(
            controls_device, task_slots_device, completion_slots_device,
            lane_capacity_, lane_capacity_mask_, stage_buffer_base_);
        break;
    case 32:
        dynamic_sharded_spsc_kernel<32><<<1, lane_count_ * 32>>>(
            controls_device, task_slots_device, completion_slots_device,
            lane_capacity_, lane_capacity_mask_, stage_buffer_base_);
        break;
    default:
        running_ = false;
        accepting_ = false;
        return EINVAL;
    }
    const int status = cuda_status(cudaGetLastError());
    if (status != 0) {
        running_ = false;
        accepting_ = false;
    }
    return status;
}

int DynamicShardedSpscQueue::try_submit(const CopyTask &task) noexcept {
    std::size_t submitted_count = 0;
    return try_submit_batch(&task, 1, &submitted_count);
}

int DynamicShardedSpscQueue::try_submit_batch(
    const CopyTask *tasks, std::size_t task_count,
    std::size_t *submitted_count) noexcept {
    if (submitted_count != nullptr) {
        *submitted_count = 0;
    }
    if (!running_ || !accepting_ || tasks == nullptr || task_count == 0 ||
        submitted_count == nullptr) {
        return EINVAL;
    }
    const std::size_t outstanding =
        static_cast<std::size_t>(accepted_tasks_ - completed_tasks_);
    if (outstanding >= capacity_) {
        return EAGAIN;
    }
    const std::size_t available = capacity_ - outstanding;
    const std::size_t count =
        task_count < available ? task_count : available;
    for (std::size_t index = 0; index < count; ++index) {
        if (tasks[index].target_address == 0 || tasks[index].length == 0) {
            return EINVAL;
        }
    }

    auto *const controls =
        dynamic_sharded_spsc_controls(memory_.host_data());
    auto *const slots = dynamic_sharded_spsc_task_slots(
        memory_.host_data(), lane_count_);
    std::uint32_t dirty_lanes = 0;
    for (std::size_t index = 0; index < count; ++index) {
        std::uint32_t selected_lane = submit_cursor_;
        bool found_lane = false;
        for (std::uint32_t visited = 0; visited < lane_count_; ++visited) {
            const std::uint64_t lane_outstanding =
                submit_tails_[selected_lane] -
                completion_heads_[selected_lane];
            if (lane_outstanding < lane_capacity_) {
                found_lane = true;
                break;
            }
            ++selected_lane;
            if (selected_lane == lane_count_) {
                selected_lane = 0;
            }
        }
        if (!found_lane) {
            return EIO;
        }
        const std::uint64_t slot_index =
            static_cast<std::uint64_t>(selected_lane) * lane_capacity_ +
            (submit_tails_[selected_lane] & lane_capacity_mask_);
        slots[slot_index].task = tasks[index];
        ++submit_tails_[selected_lane];
        dirty_lanes |= UINT32_C(1) << selected_lane;
        submit_cursor_ = selected_lane + 1;
        if (submit_cursor_ == lane_count_) {
            submit_cursor_ = 0;
        }
    }
    for (std::uint32_t lane = 0; lane < lane_count_; ++lane) {
        if ((dirty_lanes & (UINT32_C(1) << lane)) != 0) {
            host_store_release(&controls[lane].task_tail,
                               submit_tails_[lane]);
        }
    }
    accepted_tasks_ += count;
    *submitted_count = count;
    return 0;
}

int DynamicShardedSpscQueue::try_poll(
    CopyCompletion *completion) noexcept {
    if (completion == nullptr || empty()) {
        return EINVAL;
    }
    auto *const controls =
        dynamic_sharded_spsc_controls(memory_.host_data());
    auto *const slots = dynamic_sharded_spsc_completion_slots(
        memory_.host_data(), lane_count_, capacity_);
    std::uint32_t selected_lane = completion_cursor_;
    for (std::uint32_t visited = 0; visited < lane_count_; ++visited) {
        const std::uint64_t completion_tail =
            host_load_acquire(&controls[selected_lane].completion_tail);
        if (completion_heads_[selected_lane] < completion_tail) {
            const std::uint64_t slot_index =
                static_cast<std::uint64_t>(selected_lane) *
                    lane_capacity_ +
                (completion_heads_[selected_lane] &
                 lane_capacity_mask_);
            *completion = slots[slot_index].completion;
            ++completion_heads_[selected_lane];
            ++completed_tasks_;
            completion_cursor_ = selected_lane + 1;
            if (completion_cursor_ == lane_count_) {
                completion_cursor_ = 0;
            }
            return 0;
        }
        ++selected_lane;
        if (selected_lane == lane_count_) {
            selected_lane = 0;
        }
    }
    return EAGAIN;
}

int DynamicShardedSpscQueue::request_stop() noexcept {
    if (!running_ || !accepting_) {
        return EINVAL;
    }
    accepting_ = false;
    auto *const controls =
        dynamic_sharded_spsc_controls(memory_.host_data());
    for (std::uint32_t lane = 0; lane < lane_count_; ++lane) {
        host_store_release(&controls[lane].stop_requested, 1);
    }
    return 0;
}

int DynamicShardedSpscQueue::wait() noexcept {
    if (!running_ || accepting_) {
        return EINVAL;
    }
    const int status = cuda_status(cudaDeviceSynchronize());
    running_ = false;
    return status;
}

int DynamicShardedSpscQueue::reset() noexcept {
    if (running_) {
        return EBUSY;
    }
    const int status = memory_.reset();
    capacity_ = 0;
    lane_capacity_ = 0;
    lane_capacity_mask_ = 0;
    lane_count_ = 0;
    device_batch_ = 0;
    submit_cursor_ = 0;
    completion_cursor_ = 0;
    stage_buffer_base_ = 0;
    std::memset(submit_tails_, 0, sizeof(submit_tails_));
    std::memset(completion_heads_, 0, sizeof(completion_heads_));
    accepted_tasks_ = 0;
    completed_tasks_ = 0;
    accepting_ = false;
    return status;
}

std::size_t DynamicShardedSpscQueue::capacity() const noexcept {
    return capacity_;
}

std::size_t DynamicShardedSpscQueue::lane_capacity() const noexcept {
    return lane_capacity_;
}

std::size_t DynamicShardedSpscQueue::host_meta_bytes() const noexcept {
    return memory_.size();
}

std::uint32_t DynamicShardedSpscQueue::lane_count() const noexcept {
    return lane_count_;
}

std::uint32_t DynamicShardedSpscQueue::device_batch() const noexcept {
    return device_batch_;
}

std::uint64_t DynamicShardedSpscQueue::accepted_tasks() const noexcept {
    return accepted_tasks_;
}

std::uint64_t DynamicShardedSpscQueue::completed_tasks() const noexcept {
    return completed_tasks_;
}

std::uint64_t
DynamicShardedSpscQueue::host_system_atomic_operations() const noexcept {
    return 0;
}

bool DynamicShardedSpscQueue::running() const noexcept {
    return running_;
}

bool DynamicShardedSpscQueue::accepting() const noexcept {
    return accepting_;
}

bool DynamicShardedSpscQueue::drained() const noexcept {
    return accepted_tasks_ == completed_tasks_;
}

bool DynamicShardedSpscQueue::empty() const noexcept {
    return memory_.empty();
}

StaticPartitionSpscQueue::~StaticPartitionSpscQueue() {
    if (running_) {
        (void)request_stop();
        (void)wait();
    }
    (void)reset();
}

int StaticPartitionSpscQueue::allocate(
    std::size_t capacity, std::uint32_t copy_warps,
    std::uint32_t device_batch, std::uint64_t stage_buffer_base,
    StaticPartitionSpscQueue *queue) noexcept {
    std::size_t bytes = 0;
    if (queue == nullptr || !queue->empty() ||
        !is_supported_device_batch(device_batch) ||
        device_batch > capacity || stage_buffer_base == 0 ||
        !static_partition_spsc_allocation_size(capacity, copy_warps,
                                               &bytes)) {
        return EINVAL;
    }
    const int status = MappedPinnedMemory::allocate(bytes, &queue->memory_);
    if (status != 0) {
        return status;
    }
    queue->capacity_ = capacity;
    queue->capacity_mask_ = capacity - 1;
    queue->partition_capacity_ = capacity / copy_warps;
    queue->copy_warps_ = copy_warps;
    queue->device_batch_ = device_batch;
    queue->stage_buffer_base_ = stage_buffer_base;
    return 0;
}

int StaticPartitionSpscQueue::start() noexcept {
    if (empty() || running_) {
        return EINVAL;
    }
    std::memset(memory_.host_data(), 0, memory_.size());
    submit_tail_ = 0;
    completion_head_ = 0;
    accepting_ = true;
    running_ = true;

    const std::uintptr_t device_base =
        static_cast<std::uintptr_t>(memory_.device_address());
    auto *const control_device =
        reinterpret_cast<StaticPartitionSpscControl *>(device_base);
    auto *const task_slots_device =
        reinterpret_cast<StaticPartitionSpscTaskSlot *>(
            device_base + sizeof(StaticPartitionSpscControl));
    auto *const completion_slots_device =
        reinterpret_cast<StaticPartitionSpscCompletionSlot *>(
            device_base + sizeof(StaticPartitionSpscControl) +
            capacity_ * sizeof(StaticPartitionSpscTaskSlot));
    std::uint32_t copy_warp_shift = 0;
    for (std::uint32_t warps = copy_warps_; warps > 1; warps >>= 1) {
        ++copy_warp_shift;
    }
    switch (device_batch_) {
    case 1:
        static_partition_spsc_kernel<1><<<1, copy_warps_ * 32>>>(
            control_device, task_slots_device, completion_slots_device,
            capacity_mask_, copy_warps_, copy_warp_shift, stage_buffer_base_);
        break;
    case 2:
        static_partition_spsc_kernel<2><<<1, copy_warps_ * 32>>>(
            control_device, task_slots_device, completion_slots_device,
            capacity_mask_, copy_warps_, copy_warp_shift, stage_buffer_base_);
        break;
    case 4:
        static_partition_spsc_kernel<4><<<1, copy_warps_ * 32>>>(
            control_device, task_slots_device, completion_slots_device,
            capacity_mask_, copy_warps_, copy_warp_shift, stage_buffer_base_);
        break;
    case 8:
        static_partition_spsc_kernel<8><<<1, copy_warps_ * 32>>>(
            control_device, task_slots_device, completion_slots_device,
            capacity_mask_, copy_warps_, copy_warp_shift, stage_buffer_base_);
        break;
    case 16:
        static_partition_spsc_kernel<16><<<1, copy_warps_ * 32>>>(
            control_device, task_slots_device, completion_slots_device,
            capacity_mask_, copy_warps_, copy_warp_shift, stage_buffer_base_);
        break;
    case 32:
        static_partition_spsc_kernel<32><<<1, copy_warps_ * 32>>>(
            control_device, task_slots_device, completion_slots_device,
            capacity_mask_, copy_warps_, copy_warp_shift, stage_buffer_base_);
        break;
    default:
        running_ = false;
        accepting_ = false;
        return EINVAL;
    }
    const int status = cuda_status(cudaGetLastError());
    if (status != 0) {
        running_ = false;
        accepting_ = false;
    }
    return status;
}

int StaticPartitionSpscQueue::try_submit(const CopyTask &task) noexcept {
    std::size_t submitted_count = 0;
    return try_submit_batch(&task, 1, &submitted_count);
}

int StaticPartitionSpscQueue::try_submit_batch(
    const CopyTask *tasks, std::size_t task_count,
    std::size_t *submitted_count) noexcept {
    if (submitted_count != nullptr) {
        *submitted_count = 0;
    }
    if (!running_ || !accepting_ || tasks == nullptr || task_count == 0 ||
        submitted_count == nullptr) {
        return EINVAL;
    }
    const std::size_t outstanding =
        static_cast<std::size_t>(submit_tail_ - completion_head_);
    if (outstanding >= capacity_) {
        return EAGAIN;
    }
    const std::size_t available = capacity_ - outstanding;
    const std::size_t count =
        task_count < available ? task_count : available;
    for (std::size_t index = 0; index < count; ++index) {
        if (tasks[index].target_address == 0 || tasks[index].length == 0) {
            return EINVAL;
        }
    }

    auto *const control =
        static_partition_spsc_control(memory_.host_data());
    auto *const slots =
        static_partition_spsc_task_slots(memory_.host_data());
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint64_t task_index = submit_tail_ + index;
        StaticPartitionSpscTaskSlot *const slot =
            &slots[task_index & capacity_mask_];
        slot->task = tasks[index];
    }
    submit_tail_ += count;
    host_store_release(&control->task_tail, submit_tail_);
    *submitted_count = count;
    return 0;
}

int StaticPartitionSpscQueue::try_poll(
    CopyCompletion *completion) noexcept {
    if (completion == nullptr || empty()) {
        return EINVAL;
    }
    auto *const slots = static_partition_spsc_completion_slots(
        memory_.host_data(), capacity_);
    StaticPartitionSpscCompletionSlot *const slot =
        &slots[completion_head_ & capacity_mask_];
    if (host_load_acquire(&slot->sequence) != completion_head_ + 1) {
        return EAGAIN;
    }
    *completion = slot->completion;
    ++completion_head_;
    return 0;
}

int StaticPartitionSpscQueue::request_stop() noexcept {
    if (!running_ || !accepting_) {
        return EINVAL;
    }
    accepting_ = false;
    auto *const control =
        static_partition_spsc_control(memory_.host_data());
    host_store_release(&control->stop_requested, 1);
    return 0;
}

int StaticPartitionSpscQueue::wait() noexcept {
    if (!running_ || accepting_) {
        return EINVAL;
    }
    const int status = cuda_status(cudaDeviceSynchronize());
    running_ = false;
    return status;
}

int StaticPartitionSpscQueue::reset() noexcept {
    if (running_) {
        return EBUSY;
    }
    const int status = memory_.reset();
    capacity_ = 0;
    capacity_mask_ = 0;
    partition_capacity_ = 0;
    copy_warps_ = 0;
    device_batch_ = 0;
    stage_buffer_base_ = 0;
    submit_tail_ = 0;
    completion_head_ = 0;
    accepting_ = false;
    return status;
}

std::size_t StaticPartitionSpscQueue::capacity() const noexcept {
    return capacity_;
}

std::size_t StaticPartitionSpscQueue::partition_capacity() const noexcept {
    return partition_capacity_;
}

std::size_t StaticPartitionSpscQueue::host_meta_bytes() const noexcept {
    return memory_.size();
}

std::uint32_t StaticPartitionSpscQueue::copy_warps() const noexcept {
    return copy_warps_;
}

std::uint32_t StaticPartitionSpscQueue::device_batch() const noexcept {
    return device_batch_;
}

std::uint64_t StaticPartitionSpscQueue::accepted_tasks() const noexcept {
    return submit_tail_;
}

std::uint64_t StaticPartitionSpscQueue::completed_tasks() const noexcept {
    return completion_head_;
}

std::uint64_t
StaticPartitionSpscQueue::host_system_atomic_operations() const noexcept {
    return 0;
}

bool StaticPartitionSpscQueue::head_of_line_blocked() const noexcept {
    if (empty() || completion_head_ >= submit_tail_) {
        return false;
    }
    const auto *const slots = static_partition_spsc_completion_slots(
        memory_.host_data(), capacity_);
    const auto *const head_slot =
        &slots[completion_head_ & capacity_mask_];
    if (host_load_acquire(&head_slot->sequence) ==
        completion_head_ + 1) {
        return false;
    }
    for (std::uint64_t index = completion_head_ + 1;
         index < submit_tail_; ++index) {
        const auto *const slot = &slots[index & capacity_mask_];
        if (host_load_acquire(&slot->sequence) == index + 1) {
            return true;
        }
    }
    return false;
}

bool StaticPartitionSpscQueue::running() const noexcept {
    return running_;
}

bool StaticPartitionSpscQueue::accepting() const noexcept {
    return accepting_;
}

bool StaticPartitionSpscQueue::drained() const noexcept {
    return submit_tail_ == completion_head_;
}

bool StaticPartitionSpscQueue::empty() const noexcept {
    return memory_.empty();
}

WarpSpecializedQueue::~WarpSpecializedQueue() {
    if (running_) {
        (void)request_stop();
        (void)wait();
    }
    (void)reset();
}

int WarpSpecializedQueue::allocate(
    std::size_t capacity, std::uint32_t copy_warps,
    std::uint32_t device_batch,
    std::uint32_t shared_queue_depth,
    std::uint64_t stage_buffer_base,
    WarpSpecializedQueue *queue) noexcept {
    std::size_t host_bytes = 0;
    std::size_t shared_bytes = 0;
    if (queue == nullptr || !queue->empty() ||
        stage_buffer_base == 0 ||
        !warp_specialized_allocation_size(
            capacity, copy_warps, device_batch,
            shared_queue_depth, &host_bytes, &shared_bytes)) {
        return EINVAL;
    }
    const int status =
        MappedPinnedMemory::allocate(host_bytes, &queue->memory_);
    if (status != 0) {
        return status;
    }
    queue->capacity_ = capacity;
    queue->capacity_mask_ = capacity - 1;
    queue->dynamic_shared_memory_bytes_ = shared_bytes;
    queue->copy_warps_ = copy_warps;
    queue->device_batch_ = device_batch;
    queue->shared_queue_depth_ = shared_queue_depth;
    queue->stage_buffer_base_ = stage_buffer_base;
    return 0;
}

int WarpSpecializedQueue::allocate_pipeline(
    std::size_t capacity, std::uint32_t copy_warps,
    std::uint32_t device_batch,
    std::uint32_t shared_queue_depth,
    std::uint64_t stage_buffer_base,
    WarpSpecializedQueue *queue) noexcept {
    std::size_t host_bytes = 0;
    std::size_t shared_bytes = 0;
    if (queue == nullptr || !queue->empty() ||
        stage_buffer_base == 0 ||
        !warp_specialized_pipeline_allocation_size(
            capacity, copy_warps, device_batch,
            shared_queue_depth, &host_bytes, &shared_bytes)) {
        return EINVAL;
    }
    const int status =
        MappedPinnedMemory::allocate(host_bytes, &queue->memory_);
    if (status != 0) {
        return status;
    }
    queue->capacity_ = capacity;
    queue->capacity_mask_ = capacity - 1;
    queue->dynamic_shared_memory_bytes_ = shared_bytes;
    queue->copy_warps_ = copy_warps;
    queue->device_batch_ = device_batch;
    queue->shared_queue_depth_ = shared_queue_depth;
    queue->stage_buffer_base_ = stage_buffer_base;
    return 0;
}

int WarpSpecializedQueue::start() noexcept {
    return start_impl(false);
}

int WarpSpecializedQueue::start_pipeline() noexcept {
    return start_impl(true);
}

int WarpSpecializedQueue::start_impl(
    bool use_pipeline) noexcept {
    if (empty() || running_) {
        return EINVAL;
    }
    std::memset(memory_.host_data(), 0, memory_.size());
    submit_tail_ = 0;
    completion_head_ = 0;
    cached_completion_tail_ = 0;
    accepting_ = true;
    running_ = true;

    const std::uintptr_t device_base =
        static_cast<std::uintptr_t>(memory_.device_address());
    auto *const control_device =
        reinterpret_cast<WarpSpecializedControl *>(device_base);
    auto *const task_slots_device =
        reinterpret_cast<WarpSpecializedTaskSlot *>(
            device_base + sizeof(WarpSpecializedControl));
    auto *const completion_slots_device =
        reinterpret_cast<WarpSpecializedCompletionSlot *>(
            device_base + sizeof(WarpSpecializedControl) +
            capacity_ * sizeof(WarpSpecializedTaskSlot));
    const std::uint32_t thread_count =
        (copy_warps_ + 2) * 32;
    bool launched = false;
    switch (shared_queue_depth_) {
    case 2:
        launched = dispatch_warp_specialized_variant<2>(
            use_pipeline, device_batch_, control_device,
            task_slots_device,
            completion_slots_device, capacity_mask_,
            copy_warps_, stage_buffer_base_, thread_count,
            dynamic_shared_memory_bytes_);
        break;
    case 4:
        launched = dispatch_warp_specialized_variant<4>(
            use_pipeline, device_batch_, control_device,
            task_slots_device,
            completion_slots_device, capacity_mask_,
            copy_warps_, stage_buffer_base_, thread_count,
            dynamic_shared_memory_bytes_);
        break;
    case 8:
        launched = dispatch_warp_specialized_variant<8>(
            use_pipeline, device_batch_, control_device,
            task_slots_device,
            completion_slots_device, capacity_mask_,
            copy_warps_, stage_buffer_base_, thread_count,
            dynamic_shared_memory_bytes_);
        break;
    case 16:
        launched = dispatch_warp_specialized_variant<16>(
            use_pipeline, device_batch_, control_device,
            task_slots_device,
            completion_slots_device, capacity_mask_,
            copy_warps_, stage_buffer_base_, thread_count,
            dynamic_shared_memory_bytes_);
        break;
    case 32:
        launched = dispatch_warp_specialized_variant<32>(
            use_pipeline, device_batch_, control_device,
            task_slots_device,
            completion_slots_device, capacity_mask_,
            copy_warps_, stage_buffer_base_, thread_count,
            dynamic_shared_memory_bytes_);
        break;
    case 64:
        launched = dispatch_warp_specialized_variant<64>(
            use_pipeline, device_batch_, control_device,
            task_slots_device,
            completion_slots_device, capacity_mask_,
            copy_warps_, stage_buffer_base_, thread_count,
            dynamic_shared_memory_bytes_);
        break;
    case 128:
        launched = dispatch_warp_specialized_variant<128>(
            use_pipeline, device_batch_, control_device,
            task_slots_device,
            completion_slots_device, capacity_mask_,
            copy_warps_, stage_buffer_base_, thread_count,
            dynamic_shared_memory_bytes_);
        break;
    case 256:
        launched = dispatch_warp_specialized_variant<256>(
            use_pipeline, device_batch_, control_device,
            task_slots_device,
            completion_slots_device, capacity_mask_,
            copy_warps_, stage_buffer_base_, thread_count,
            dynamic_shared_memory_bytes_);
        break;
    case 512:
        launched = dispatch_warp_specialized_variant<512>(
            use_pipeline, device_batch_, control_device,
            task_slots_device,
            completion_slots_device, capacity_mask_,
            copy_warps_, stage_buffer_base_, thread_count,
            dynamic_shared_memory_bytes_);
        break;
    default:
        break;
    }
    if (!launched) {
        running_ = false;
        accepting_ = false;
        return EINVAL;
    }
    const int status = cuda_status(cudaGetLastError());
    if (status != 0) {
        running_ = false;
        accepting_ = false;
    }
    return status;
}

int WarpSpecializedQueue::try_submit(
    const CopyTask &task) noexcept {
    std::size_t submitted_count = 0;
    return try_submit_batch(&task, 1, &submitted_count);
}

int WarpSpecializedQueue::try_submit_batch(
    const CopyTask *tasks, std::size_t task_count,
    std::size_t *submitted_count) noexcept {
    if (submitted_count != nullptr) {
        *submitted_count = 0;
    }
    if (!running_ || !accepting_ || tasks == nullptr ||
        task_count == 0 || submitted_count == nullptr) {
        return EINVAL;
    }
    const std::size_t outstanding =
        static_cast<std::size_t>(
            submit_tail_ - completion_head_);
    if (outstanding >= capacity_) {
        return EAGAIN;
    }
    const std::size_t available = capacity_ - outstanding;
    const std::size_t count =
        task_count < available ? task_count : available;
    for (std::size_t index = 0; index < count; ++index) {
        if (tasks[index].target_address == 0 ||
            tasks[index].length == 0) {
            return EINVAL;
        }
    }

    auto *const control =
        warp_specialized_control(memory_.host_data());
    auto *const slots =
        warp_specialized_task_slots(memory_.host_data());
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint64_t task_index = submit_tail_ + index;
        slots[task_index & capacity_mask_].task = tasks[index];
    }
    submit_tail_ += count;
    host_store_release(&control->task_tail, submit_tail_);
    *submitted_count = count;
    return 0;
}

int WarpSpecializedQueue::try_poll(
    CopyCompletion *completion) noexcept {
    if (completion == nullptr || empty()) {
        return EINVAL;
    }
    auto *const control =
        warp_specialized_control(memory_.host_data());
    if (completion_head_ >= cached_completion_tail_) {
        cached_completion_tail_ =
            host_load_acquire(&control->completion_tail);
        if (completion_head_ >= cached_completion_tail_) {
            return EAGAIN;
        }
    }
    auto *const slots = warp_specialized_completion_slots(
        memory_.host_data(), capacity_);
    *completion =
        slots[completion_head_ & capacity_mask_].completion;
    ++completion_head_;
    return 0;
}

int WarpSpecializedQueue::request_stop() noexcept {
    if (!running_ || !accepting_) {
        return EINVAL;
    }
    accepting_ = false;
    auto *const control =
        warp_specialized_control(memory_.host_data());
    host_store_release(&control->stop_requested, 1);
    return 0;
}

int WarpSpecializedQueue::wait() noexcept {
    if (!running_ || accepting_) {
        return EINVAL;
    }
    const int status =
        cuda_status(cudaDeviceSynchronize());
    running_ = false;
    return status;
}

int WarpSpecializedQueue::reset() noexcept {
    if (running_) {
        return EBUSY;
    }
    const int status = memory_.reset();
    capacity_ = 0;
    capacity_mask_ = 0;
    dynamic_shared_memory_bytes_ = 0;
    copy_warps_ = 0;
    device_batch_ = 0;
    shared_queue_depth_ = 0;
    stage_buffer_base_ = 0;
    submit_tail_ = 0;
    completion_head_ = 0;
    cached_completion_tail_ = 0;
    accepting_ = false;
    return status;
}

std::size_t WarpSpecializedQueue::capacity() const noexcept {
    return capacity_;
}

std::size_t
WarpSpecializedQueue::host_meta_bytes() const noexcept {
    return memory_.size();
}

std::size_t
WarpSpecializedQueue::dynamic_shared_memory_bytes() const noexcept {
    return dynamic_shared_memory_bytes_;
}

std::uint32_t WarpSpecializedQueue::copy_warps() const noexcept {
    return copy_warps_;
}

std::uint32_t WarpSpecializedQueue::device_batch() const noexcept {
    return device_batch_;
}

std::uint32_t
WarpSpecializedQueue::shared_queue_depth() const noexcept {
    return shared_queue_depth_;
}

std::uint64_t
WarpSpecializedQueue::accepted_tasks() const noexcept {
    return submit_tail_;
}

std::uint64_t
WarpSpecializedQueue::completed_tasks() const noexcept {
    return completion_head_;
}

std::uint64_t
WarpSpecializedQueue::host_system_atomic_operations() const noexcept {
    return 0;
}

bool WarpSpecializedQueue::running() const noexcept {
    return running_;
}

bool WarpSpecializedQueue::accepting() const noexcept {
    return accepting_;
}

bool WarpSpecializedQueue::drained() const noexcept {
    return submit_tail_ == completion_head_;
}

bool WarpSpecializedQueue::empty() const noexcept {
    return memory_.empty();
}

int PersistentCopyLifecycle::start(const PersistentCopyConfig &config) noexcept {
    if (state_ != PersistentCopyLifecycleState::stopped) {
        return EBUSY;
    }
    const int status = validate_persistent_copy_config(config);
    if (status != 0) {
        return status;
    }
    config_ = config;
    accepted_tasks_ = 0;
    completed_tasks_ = 0;
    state_ = PersistentCopyLifecycleState::accepting;
    return 0;
}

int PersistentCopyLifecycle::record_accepted(std::uint64_t count) noexcept {
    if (state_ != PersistentCopyLifecycleState::accepting || count == 0) {
        return EINVAL;
    }
    const std::uint64_t outstanding = accepted_tasks_ - completed_tasks_;
    if (outstanding > config_.outstanding_capacity ||
        count > config_.outstanding_capacity - outstanding) {
        return EAGAIN;
    }
    if (accepted_tasks_ > std::numeric_limits<std::uint64_t>::max() - count) {
        return EOVERFLOW;
    }
    accepted_tasks_ += count;
    return 0;
}

int PersistentCopyLifecycle::record_completed(std::uint64_t count) noexcept {
    if (state_ == PersistentCopyLifecycleState::stopped || count == 0 ||
        completed_tasks_ > accepted_tasks_ || count > accepted_tasks_ - completed_tasks_) {
        return EINVAL;
    }
    completed_tasks_ += count;
    return 0;
}

int PersistentCopyLifecycle::request_stop() noexcept {
    if (state_ != PersistentCopyLifecycleState::accepting) {
        return EINVAL;
    }
    state_ = PersistentCopyLifecycleState::draining;
    return 0;
}

int PersistentCopyLifecycle::finish_stop() noexcept {
    if (state_ != PersistentCopyLifecycleState::draining) {
        return EINVAL;
    }
    if (!drained()) {
        return EAGAIN;
    }
    state_ = PersistentCopyLifecycleState::stopped;
    return 0;
}

PersistentCopyLifecycleState PersistentCopyLifecycle::state() const noexcept {
    return state_;
}

const PersistentCopyConfig &PersistentCopyLifecycle::config() const noexcept {
    return config_;
}

std::uint64_t PersistentCopyLifecycle::accepted_tasks() const noexcept {
    return accepted_tasks_;
}

std::uint64_t PersistentCopyLifecycle::completed_tasks() const noexcept {
    return completed_tasks_;
}

bool PersistentCopyLifecycle::drained() const noexcept {
    return accepted_tasks_ == completed_tasks_;
}

}  // namespace ugdr::gpu

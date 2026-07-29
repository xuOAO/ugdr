#include "gpu/persistent_copy.hpp"

#include <cuda.h>
#include <cuda_runtime_api.h>

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
    for (std::uint32_t unit = lane; unit < unit_count; unit += 32) {
        const std::uint32_t offset = unit * Width;
        copy_cg_narrow(source + offset, target + offset, Width);
    }
}

__device__ __forceinline__ void copy_cg_warp(const std::uint8_t *source,
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
};

static_assert(sizeof(DirectAtomicControl) == 64);
static_assert(sizeof(DirectAtomicTaskSlot) == 32);
static_assert(sizeof(DirectAtomicCompletionSlot) == 32);

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

__global__ void direct_atomic_kernel(DirectAtomicControl *control,
                                     DirectAtomicTaskSlot *task_slots,
                                     DirectAtomicCompletionSlot *completion_slots,
                                     std::uint64_t capacity,
                                     std::uint64_t stage_buffer_base) {
    constexpr std::uint64_t kNoTask = UINT64_MAX;
    constexpr unsigned kFullWarp = 0xffffffffU;
    const std::uint32_t lane = threadIdx.x & 31U;

    while (true) {
        std::uint64_t task_index = kNoTask;
        bool should_stop = false;
        if (lane == 0) {
            const std::uint64_t tail = system_load_acquire(&control->task_tail);
            const std::uint64_t head = system_load_acquire(&control->task_head);
            if (head < tail &&
                system_atomic_cas(&control->task_head, head, head + 1) == head) {
                task_index = head;
            } else if (head >= tail &&
                       system_load_acquire(&control->stop_requested) != 0) {
                const std::uint64_t final_tail =
                    system_load_acquire(&control->task_tail);
                const std::uint64_t final_head =
                    system_load_acquire(&control->task_head);
                should_stop = final_head >= final_tail;
            }
        }
        task_index = __shfl_sync(kFullWarp, task_index, 0);
        should_stop = __shfl_sync(kFullWarp, should_stop, 0);
        if (should_stop) {
            return;
        }
        if (task_index == kNoTask) {
            if (lane == 0) {
                __nanosleep(64);
            }
            continue;
        }

        DirectAtomicTaskSlot *const task_slot = &task_slots[task_index % capacity];
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
            const std::uint64_t completion_index =
                system_atomic_add(&control->completion_tail, 1);
            DirectAtomicCompletionSlot *const completion_slot =
                &completion_slots[completion_index % capacity];
            completion_slot->completion.task_id = task.task_id;
            completion_slot->completion.result = CopyTaskResult::success;
            system_store_release(&completion_slot->sequence, completion_index + 1);
        }
    }
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

bool direct_atomic_allocation_size(std::size_t capacity, std::size_t *bytes) noexcept {
    constexpr std::size_t kPerSlotBytes =
        sizeof(DirectAtomicTaskSlot) + sizeof(DirectAtomicCompletionSlot);
    if (capacity == 0 ||
        capacity > (std::numeric_limits<std::size_t>::max() - sizeof(DirectAtomicControl)) /
                       kPerSlotBytes) {
        return false;
    }
    *bytes = sizeof(DirectAtomicControl) + capacity * kPerSlotBytes;
    return true;
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
        break;
    default:
        return EINVAL;
    }
    if (config.payload_bytes == 0 || config.payload_bytes > kPersistentCopyMaxPayloadBytes ||
        config.parent_wr_bytes == 0 || config.outstanding_capacity == 0 || config.host_batch == 0 ||
        config.host_batch > config.outstanding_capacity || config.copy_warps == 0 ||
        config.copy_warps > 32 || config.warmup_tasks == 0 || config.iterations == 0) {
        return EINVAL;
    }
    if (config.model == PersistentCopyModel::warp_specialized &&
        (config.copy_warps > 30 || config.shared_stage_count == 0)) {
        return EINVAL;
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
                                std::uint64_t stage_buffer_base,
                                DirectAtomicQueue *queue) noexcept {
    std::size_t bytes = 0;
    if (queue == nullptr || !queue->empty() || copy_warps == 0 || copy_warps > 32 ||
        stage_buffer_base == 0 || !direct_atomic_allocation_size(capacity, &bytes)) {
        return EINVAL;
    }
    const int status = MappedPinnedMemory::allocate(bytes, &queue->memory_);
    if (status != 0) {
        return status;
    }
    queue->capacity_ = capacity;
    queue->copy_warps_ = copy_warps;
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
    direct_atomic_kernel<<<1, copy_warps_ * 32>>>(
        control_device, task_slots_device, completion_slots_device, capacity_,
        stage_buffer_base_);
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
        slots[(submit_tail_ + index) % capacity_].task = tasks[index];
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
                            [completion_head_ % capacity_];
    if (host_load_acquire(&slot->sequence) != completion_head_ + 1) {
        return EAGAIN;
    }
    *completion = slot->completion;
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
    copy_warps_ = 0;
    stage_buffer_base_ = 0;
    submit_tail_ = 0;
    completion_head_ = 0;
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

std::uint64_t DirectAtomicQueue::accepted_tasks() const noexcept {
    return submit_tail_;
}

std::uint64_t DirectAtomicQueue::completed_tasks() const noexcept {
    return completion_head_;
}

std::uint64_t DirectAtomicQueue::host_system_atomic_operations() const noexcept {
    return completion_head_ * 2;
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

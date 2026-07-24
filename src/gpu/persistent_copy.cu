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
        config.copy_warps > 30 || config.warmup_tasks == 0 || config.iterations == 0) {
        return EINVAL;
    }
    if (config.model == PersistentCopyModel::warp_specialized && config.shared_stage_count == 0) {
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
    : source_allocation_(std::exchange(other.source_allocation_, nullptr)),
      target_allocation_(std::exchange(other.target_allocation_, nullptr)),
      payload_capacity_(std::exchange(other.payload_capacity_, 0)),
      guard_bytes_(std::exchange(other.guard_bytes_, 0)) {
}

PersistentCopyPayloadBuffer &
PersistentCopyPayloadBuffer::operator=(PersistentCopyPayloadBuffer &&other) noexcept {
    if (this != &other) {
        (void)reset();
        source_allocation_ = std::exchange(other.source_allocation_, nullptr);
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
        !checked_allocation_size(payload_capacity, guard_bytes, &allocation_size)) {
        return EINVAL;
    }
    void *source = nullptr;
    cudaError_t status = cudaMalloc(&source, allocation_size);
    if (status != cudaSuccess) {
        return cuda_status(status);
    }
    void *target = nullptr;
    status = cudaMalloc(&target, allocation_size);
    if (status != cudaSuccess) {
        (void)cudaFree(source);
        return cuda_status(status);
    }
    buffer->source_allocation_ = source;
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
        std::vector<std::uint8_t> source(allocation_size, kPayloadGuardByte);
        std::vector<std::uint8_t> target(allocation_size, kPayloadGuardByte);
        for (std::size_t index = 0; index < payload_capacity_; ++index) {
            source[guard_bytes_ + index] = persistent_copy_payload_byte(seed, index);
            target[guard_bytes_ + index] = kPayloadInitialTargetByte;
        }
        CUresult status =
            cuMemcpyHtoD(static_cast<CUdeviceptr>(pointer_address(source_allocation_)),
                         source.data(), source.size());
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
    std::size_t allocation_size = 0;
    if (check == nullptr || empty() ||
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
            if (observed[guard_bytes_ + index] != persistent_copy_payload_byte(seed, index)) {
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

int PersistentCopyPayloadBuffer::make_task(std::uint64_t task_id, std::uint64_t parent_request_id,
                                           std::uint32_t payload_index, std::size_t length,
                                           CopyTask *task) const noexcept {
    if (task == nullptr || empty() || length == 0 || length > payload_capacity_ ||
        length > std::numeric_limits<std::uint32_t>::max()) {
        return EINVAL;
    }
    CopyTask result;
    result.task_id = task_id;
    result.parent_request_id = parent_request_id;
    result.payload_index = payload_index;
    result.source_address = source_address();
    result.target_address = target_address();
    result.length = static_cast<std::uint32_t>(length);
    *task = result;
    return 0;
}

int PersistentCopyPayloadBuffer::reset() noexcept {
    void *const source = std::exchange(source_allocation_, nullptr);
    void *const target = std::exchange(target_allocation_, nullptr);
    payload_capacity_ = 0;
    guard_bytes_ = 0;
    int status = 0;
    if (source != nullptr) {
        status = cuda_status(cudaFree(source));
    }
    if (target != nullptr) {
        const int target_status = cuda_status(cudaFree(target));
        if (status == 0) {
            status = target_status;
        }
    }
    return status;
}

std::uint64_t PersistentCopyPayloadBuffer::source_address() const noexcept {
    if (source_allocation_ == nullptr) {
        return 0;
    }
    return pointer_address(static_cast<const std::uint8_t *>(source_allocation_) + guard_bytes_);
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
    return source_allocation_ == nullptr && target_allocation_ == nullptr;
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

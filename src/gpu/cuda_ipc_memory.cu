#include "gpu/cuda_ipc_memory.hpp"

#include <cuda.h>
#include <cuda_runtime_api.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

namespace ugdr::gpu {
namespace {

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

bool checked_range(std::uint64_t start, std::uint64_t length, std::uint64_t limit_start,
                   std::uint64_t limit_length) noexcept {
    if (length == 0 || start < limit_start || limit_length == 0) {
        return false;
    }
    if (start > std::numeric_limits<std::uint64_t>::max() - length ||
        limit_start > std::numeric_limits<std::uint64_t>::max() - limit_length) {
        return false;
    }
    return start + length <= limit_start + limit_length;
}

const RuntimeCudaIpcMemoryBackend::DeviceRuntime *
find_device(const std::vector<RuntimeCudaIpcMemoryBackend::DeviceRuntime> &devices,
            const GpuUuid &uuid) noexcept {
    for (const auto &device : devices) {
        if (device.uuid == uuid) {
            return &device;
        }
    }
    return nullptr;
}

class ScopedDevice {
  public:
    explicit ScopedDevice(int ordinal) noexcept {
        if (cudaGetDevice(&previous_) != cudaSuccess) {
            previous_ = -1;
            (void)cudaGetLastError();
        }
        status_ = cuda_status(cudaSetDevice(ordinal));
    }

    ~ScopedDevice() {
        if (status_ == 0 && previous_ >= 0) {
            (void)cudaSetDevice(previous_);
        }
    }

    [[nodiscard]] int status() const noexcept {
        return status_;
    }

  private:
    int previous_ = -1;
    int status_ = 0;
};

}  // namespace

int export_cuda_memory(void *address, std::size_t length, ExportedCudaMemory *memory) noexcept {
    if (address == nullptr || length == 0 || memory == nullptr) {
        return EINVAL;
    }
    const int init_status = driver_status(cuInit(0));
    if (init_status != 0) {
        return init_status;
    }

    cudaPointerAttributes attributes{};
    cudaError_t status = cudaPointerGetAttributes(&attributes, address);
    if (status != cudaSuccess) {
        (void)cudaGetLastError();
        return status == cudaErrorInvalidValue ? EOPNOTSUPP : cuda_status(status);
    }
    if (attributes.type != cudaMemoryTypeDevice || attributes.device < 0) {
        return EOPNOTSUPP;
    }

    CUdeviceptr allocation_base = 0;
    std::size_t allocation_size = 0;
    const CUresult range_status =
        cuMemGetAddressRange(&allocation_base, &allocation_size,
                             static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(address)));
    if (range_status != CUDA_SUCCESS) {
        return driver_status(range_status);
    }

    const auto client_address =
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(address));
    const auto base_address = static_cast<std::uint64_t>(allocation_base);
    if (client_address < base_address ||
        !checked_range(client_address, length, base_address,
                       static_cast<std::uint64_t>(allocation_size))) {
        return EINVAL;
    }

    CUdevice device = 0;
    CUresult device_status = cuDeviceGet(&device, attributes.device);
    if (device_status != CUDA_SUCCESS) {
        return driver_status(device_status);
    }
    CUuuid uuid{};
    device_status = cuDeviceGetUuid(&uuid, device);
    if (device_status != CUDA_SUCCESS) {
        return driver_status(device_status);
    }
    cudaIpcMemHandle_t handle{};
    status = cudaIpcGetMemHandle(
        &handle, reinterpret_cast<void *>(static_cast<std::uintptr_t>(allocation_base)));
    if (status != cudaSuccess) {
        return cuda_status(status);
    }

    ExportedCudaMemory exported;
    std::memcpy(exported.gpu_uuid.data(), uuid.bytes, exported.gpu_uuid.size());
    exported.client_address = client_address;
    exported.allocation_size = static_cast<std::uint64_t>(allocation_size);
    exported.allocation_offset = client_address - base_address;
    exported.length = static_cast<std::uint64_t>(length);
    const auto *handle_begin = reinterpret_cast<const std::byte *>(&handle);
    exported.ipc_handle.assign(handle_begin, handle_begin + sizeof(handle));
    *memory = std::move(exported);
    return 0;
}

RuntimeCudaIpcMemoryBackend::RuntimeCudaIpcMemoryBackend() noexcept {
    const CUresult driver_init = cuInit(0);
    if (driver_init != CUDA_SUCCESS) {
        initialization_status_ = driver_status(driver_init);
        return;
    }
    int count = 0;
    cudaError_t status = cudaGetDeviceCount(&count);
    if (status != cudaSuccess) {
        initialization_status_ = cuda_status(status);
        (void)cudaGetLastError();
        return;
    }
    try {
        devices_.reserve(static_cast<std::size_t>(count));
        for (int ordinal = 0; ordinal < count; ++ordinal) {
            ScopedDevice selected(ordinal);
            if (selected.status() != 0) {
                initialization_status_ = selected.status();
                devices_.clear();
                return;
            }
            status = cudaFree(nullptr);
            if (status != cudaSuccess) {
                initialization_status_ = cuda_status(status);
                devices_.clear();
                return;
            }
            CUdevice device = 0;
            CUresult device_status = cuDeviceGet(&device, ordinal);
            if (device_status != CUDA_SUCCESS) {
                initialization_status_ = driver_status(device_status);
                devices_.clear();
                return;
            }
            CUuuid uuid{};
            device_status = cuDeviceGetUuid(&uuid, device);
            if (device_status != CUDA_SUCCESS) {
                initialization_status_ = driver_status(device_status);
                devices_.clear();
                return;
            }
            DeviceRuntime runtime;
            runtime.ordinal = ordinal;
            std::memcpy(runtime.uuid.data(), uuid.bytes, runtime.uuid.size());
            devices_.push_back(runtime);
        }
    } catch (...) {
        initialization_status_ = ENOMEM;
        devices_.clear();
    }
}

int RuntimeCudaIpcMemoryBackend::open(const ExportedCudaMemory &memory, CudaIpcMapping *mapping) {
    if (mapping == nullptr || initialization_status_ != 0) {
        return mapping == nullptr ? EINVAL : initialization_status_;
    }
    if (memory.ipc_handle.size() != sizeof(cudaIpcMemHandle_t)) {
        return EINVAL;
    }
    const DeviceRuntime *const device = find_device(devices_, memory.gpu_uuid);
    if (device == nullptr) {
        return ENODEV;
    }
    ScopedDevice selected(device->ordinal);
    if (selected.status() != 0) {
        return selected.status();
    }

    cudaIpcMemHandle_t handle{};
    std::memcpy(&handle, memory.ipc_handle.data(), sizeof(handle));
    void *daemon_base = nullptr;
    const cudaError_t status =
        cudaIpcOpenMemHandle(&daemon_base, handle, cudaIpcMemLazyEnablePeerAccess);
    if (status != cudaSuccess) {
        return cuda_status(status);
    }
    CudaIpcMapping opened;
    opened.gpu_uuid = memory.gpu_uuid;
    opened.daemon_base_address =
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(daemon_base));
    *mapping = opened;
    return 0;
}

int RuntimeCudaIpcMemoryBackend::close(const CudaIpcMapping &mapping) noexcept {
    if (mapping.daemon_base_address == 0 || initialization_status_ != 0) {
        return mapping.daemon_base_address == 0 ? EINVAL : initialization_status_;
    }
    const DeviceRuntime *const device = find_device(devices_, mapping.gpu_uuid);
    if (device == nullptr) {
        return ENODEV;
    }
    ScopedDevice selected(device->ordinal);
    if (selected.status() != 0) {
        return selected.status();
    }
    void *const daemon_base =
        reinterpret_cast<void *>(static_cast<std::uintptr_t>(mapping.daemon_base_address));
    return cuda_status(cudaIpcCloseMemHandle(daemon_base));
}

int RuntimeCudaIpcMemoryBackend::fill(const CudaIpcMapping &mapping, std::uint64_t offset,
                                      std::uint64_t length, std::uint8_t value) noexcept {
    if (mapping.daemon_base_address == 0 || length == 0 ||
        mapping.daemon_base_address > std::numeric_limits<std::uint64_t>::max() - offset ||
        length > std::numeric_limits<std::size_t>::max()) {
        return EINVAL;
    }
    const DeviceRuntime *const device = find_device(devices_, mapping.gpu_uuid);
    if (device == nullptr) {
        return ENODEV;
    }
    ScopedDevice selected(device->ordinal);
    if (selected.status() != 0) {
        return selected.status();
    }
    void *const start =
        reinterpret_cast<void *>(static_cast<std::uintptr_t>(mapping.daemon_base_address + offset));
    const cudaError_t fill_status = cudaMemset(start, value, static_cast<std::size_t>(length));
    return fill_status == cudaSuccess ? cuda_status(cudaDeviceSynchronize())
                                      : cuda_status(fill_status);
}

int RuntimeCudaIpcMemoryBackend::initialization_status() const noexcept {
    return initialization_status_;
}

std::size_t RuntimeCudaIpcMemoryBackend::device_count() const noexcept {
    return devices_.size();
}

}  // namespace ugdr::gpu

#include "gpu/gpudirect_visibility.hpp"

#include <cuda.h>

#include <cerrno>
#include <limits>

namespace ugdr::gpu {
namespace {

int driver_status(CUresult status) noexcept {
    switch (status) {
    case CUDA_SUCCESS:
        return 0;
    case CUDA_ERROR_INVALID_VALUE:
    case CUDA_ERROR_INVALID_DEVICE:
    case CUDA_ERROR_INVALID_CONTEXT:
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

int get_device_attribute(CUdevice device, CUdevice_attribute attribute, int *value) noexcept {
    return driver_status(cuDeviceGetAttribute(value, attribute, device));
}

}  // namespace

int query_gpudirect_visibility_capabilities(
    std::uint32_t device_ordinal, GpuDirectVisibilityCapabilities *capabilities) noexcept {
    if (capabilities == nullptr ||
        device_ordinal > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return EINVAL;
    }
    int status = driver_status(cuInit(0));
    if (status != 0) {
        return status;
    }
    CUdevice device = 0;
    status = driver_status(cuDeviceGet(&device, static_cast<int>(device_ordinal)));
    if (status != 0) {
        return status;
    }

    int rdma_supported = 0;
    int flush_options = 0;
    int writes_ordering = 0;
    status = get_device_attribute(device, CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_SUPPORTED,
                                  &rdma_supported);
    if (status == 0) {
        status = get_device_attribute(
            device, CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_FLUSH_WRITES_OPTIONS, &flush_options);
    }
    if (status == 0) {
        status = get_device_attribute(device, CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_WRITES_ORDERING,
                                      &writes_ordering);
    }
    if (status != 0) {
        return status;
    }

    GpuDirectRdmaWritesOrdering ordering{};
    switch (writes_ordering) {
    case CU_GPU_DIRECT_RDMA_WRITES_ORDERING_NONE:
        ordering = GpuDirectRdmaWritesOrdering::none;
        break;
    case CU_GPU_DIRECT_RDMA_WRITES_ORDERING_OWNER:
        ordering = GpuDirectRdmaWritesOrdering::owner;
        break;
    case CU_GPU_DIRECT_RDMA_WRITES_ORDERING_ALL_DEVICES:
        ordering = GpuDirectRdmaWritesOrdering::all_devices;
        break;
    default:
        return EIO;
    }

    capabilities->gpudirect_rdma_supported = rdma_supported != 0;
    capabilities->host_flush_supported =
        (flush_options & CU_FLUSH_GPU_DIRECT_RDMA_WRITES_OPTION_HOST) != 0;
    capabilities->stream_memops_flush_supported =
        (flush_options & CU_FLUSH_GPU_DIRECT_RDMA_WRITES_OPTION_MEMOPS) != 0;
    capabilities->writes_ordering = ordering;
    return 0;
}

int CudaGpuDirectVisibilityGate::initialize(std::uint32_t device_ordinal) noexcept {
    if (initialized_) {
        return EINVAL;
    }
    GpuDirectVisibilityCapabilities capabilities{};
    const int status = query_gpudirect_visibility_capabilities(device_ordinal, &capabilities);
    if (status != 0) {
        return status;
    }
    if (!capabilities.gpudirect_rdma_supported) {
        return EOPNOTSUPP;
    }
    const bool flush_required = capabilities.writes_ordering < GpuDirectRdmaWritesOrdering::owner;
    if (flush_required && !capabilities.host_flush_supported) {
        return EOPNOTSUPP;
    }
    capabilities_ = capabilities;
    initialized_ = true;
    flush_required_ = flush_required;
    return 0;
}

int CudaGpuDirectVisibilityGate::flush_current_context_to_owner() noexcept {
    if (!initialized_) {
        return EINVAL;
    }
    if (!flush_required_) {
        return 0;
    }
    return driver_status(
        cuFlushGPUDirectRDMAWrites(CU_FLUSH_GPU_DIRECT_RDMA_WRITES_TARGET_CURRENT_CTX,
                                   CU_FLUSH_GPU_DIRECT_RDMA_WRITES_TO_OWNER));
}

const GpuDirectVisibilityCapabilities &CudaGpuDirectVisibilityGate::capabilities() const noexcept {
    return capabilities_;
}

bool CudaGpuDirectVisibilityGate::initialized() const noexcept {
    return initialized_;
}

bool CudaGpuDirectVisibilityGate::flush_required() const noexcept {
    return flush_required_;
}

}  // namespace ugdr::gpu

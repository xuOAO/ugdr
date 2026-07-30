#include "gpu/gpudirect_visibility.hpp"
#include "gpu/persistent_copy.hpp"

#include <cuda_runtime_api.h>

#include <cerrno>

int main() {
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
        (void)cudaGetLastError();
        return 77;
    }
    if (ugdr::gpu::initialize_persistent_copy_device(0) != 0) {
        return 1;
    }

    ugdr::gpu::GpuDirectVisibilityCapabilities capabilities{};
    if (ugdr::gpu::query_gpudirect_visibility_capabilities(0, &capabilities) != 0 ||
        !capabilities.gpudirect_rdma_supported) {
        return 2;
    }
    if (capabilities.writes_ordering == ugdr::gpu::GpuDirectRdmaWritesOrdering::none &&
        !capabilities.host_flush_supported) {
        return 3;
    }

    ugdr::gpu::CudaGpuDirectVisibilityGate gate;
    if (gate.flush_current_context_to_owner() != EINVAL || gate.initialize(0) != 0 ||
        !gate.initialized() || gate.flush_current_context_to_owner() != 0) {
        return 4;
    }
    if (gate.capabilities().gpudirect_rdma_supported != capabilities.gpudirect_rdma_supported ||
        gate.capabilities().host_flush_supported != capabilities.host_flush_supported ||
        gate.capabilities().stream_memops_flush_supported !=
            capabilities.stream_memops_flush_supported ||
        gate.capabilities().writes_ordering != capabilities.writes_ordering) {
        return 5;
    }
    return 0;
}

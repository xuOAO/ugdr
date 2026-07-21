#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ugdr::gpu {

constexpr std::size_t kGpuUuidSize = 16;
constexpr std::size_t kMaxCudaIpcHandleSize = 256;

using GpuUuid = std::array<std::uint8_t, kGpuUuidSize>;

struct ExportedCudaMemory {
    GpuUuid gpu_uuid{};
    std::uint64_t client_address = 0;
    std::uint64_t allocation_size = 0;
    std::uint64_t allocation_offset = 0;
    std::uint64_t length = 0;
    std::vector<std::byte> ipc_handle;
};

struct CudaIpcMapping {
    GpuUuid gpu_uuid{};
    std::uint64_t daemon_base_address = 0;
};

class CudaIpcMemoryBackend {
  public:
    virtual ~CudaIpcMemoryBackend() = default;

    virtual int open(const ExportedCudaMemory &memory, CudaIpcMapping *mapping) = 0;
    virtual int close(const CudaIpcMapping &mapping) noexcept = 0;
};

int export_cuda_memory(void *address, std::size_t length, ExportedCudaMemory *memory) noexcept;

class RuntimeCudaIpcMemoryBackend final : public CudaIpcMemoryBackend {
  public:
    struct DeviceRuntime {
        GpuUuid uuid{};
        int ordinal = -1;
    };

    RuntimeCudaIpcMemoryBackend() noexcept;

    int open(const ExportedCudaMemory &memory, CudaIpcMapping *mapping) override;
    int close(const CudaIpcMapping &mapping) noexcept override;
    int fill(const CudaIpcMapping &mapping, std::uint64_t offset, std::uint64_t length,
             std::uint8_t value) noexcept;

    [[nodiscard]] int initialization_status() const noexcept;
    [[nodiscard]] std::size_t device_count() const noexcept;

  private:
    int initialization_status_ = 0;
    std::vector<DeviceRuntime> devices_;
};

}  // namespace ugdr::gpu

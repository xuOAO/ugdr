#pragma once

#include <cstdint>

namespace ugdr::gpu {

enum class GpuDirectRdmaWritesOrdering : std::int32_t {
    none = 0,
    owner = 100,
    all_devices = 200,
};

struct GpuDirectVisibilityCapabilities {
    bool gpudirect_rdma_supported = false;
    bool host_flush_supported = false;
    bool stream_memops_flush_supported = false;
    GpuDirectRdmaWritesOrdering writes_ordering = GpuDirectRdmaWritesOrdering::none;
};

int query_gpudirect_visibility_capabilities(std::uint32_t device_ordinal,
                                            GpuDirectVisibilityCapabilities *capabilities) noexcept;

class GpuDirectVisibilityGate {
  public:
    virtual ~GpuDirectVisibilityGate() = default;

    virtual int initialize(std::uint32_t device_ordinal) noexcept = 0;
    virtual int flush_current_context_to_owner() noexcept = 0;
};

class CudaGpuDirectVisibilityGate final : public GpuDirectVisibilityGate {
  public:
    int initialize(std::uint32_t device_ordinal) noexcept override;
    int flush_current_context_to_owner() noexcept override;

    [[nodiscard]] const GpuDirectVisibilityCapabilities &capabilities() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] bool flush_required() const noexcept;

  private:
    GpuDirectVisibilityCapabilities capabilities_{};
    bool initialized_ = false;
    bool flush_required_ = false;
};

}  // namespace ugdr::gpu

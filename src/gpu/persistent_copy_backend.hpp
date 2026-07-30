#pragma once

#include "gpu/gpudirect_visibility.hpp"
#include "gpu/persistent_copy.hpp"
#include "worker/copy_backend.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace ugdr::gpu {

constexpr std::uint32_t kPersistentCudaCopyBackendCopyWarps = 30;
constexpr std::uint32_t kPersistentCudaCopyBackendDeviceBatch = kWarpSpecializedMetaBatch;
constexpr std::uint32_t kPersistentCudaCopyBackendSharedQueueDepth = 16;
constexpr std::size_t kPersistentCudaCopyBackendHostBatch = 64;

struct PersistentCudaCopyBackendConfig {
    std::uint32_t device_ordinal = 0;
    std::uint64_t stage_buffer_base = 0;
    std::uint64_t stage_buffer_bytes = 0;
    std::size_t queue_capacity = 1024;
    std::size_t host_batch = kPersistentCudaCopyBackendHostBatch;
    std::uint64_t max_batch_delay_nanoseconds = 0;
};

int validate_persistent_cuda_copy_backend_config(
    const PersistentCudaCopyBackendConfig &config) noexcept;

class PersistentCopyQueue {
  public:
    virtual ~PersistentCopyQueue() = default;

    virtual int try_submit_batch(const CopyTask *tasks, std::size_t task_count,
                                 std::size_t *submitted_count) noexcept = 0;
    virtual int try_poll(CopyCompletion *completion) noexcept = 0;
};

class PersistentCopyRuntime : public PersistentCopyQueue {
  public:
    virtual int initialize_device(std::uint32_t device_ordinal) noexcept = 0;
    virtual int start(const PersistentCudaCopyBackendConfig &config) noexcept = 0;
    virtual int request_stop() noexcept = 0;
    virtual int wait() noexcept = 0;
    virtual int reset() noexcept = 0;
};

class CudaPersistentCopyRuntime final : public PersistentCopyRuntime {
  public:
    int initialize_device(std::uint32_t device_ordinal) noexcept override;
    int start(const PersistentCudaCopyBackendConfig &config) noexcept override;
    int try_submit_batch(const CopyTask *tasks, std::size_t task_count,
                         std::size_t *submitted_count) noexcept override;
    int try_poll(CopyCompletion *completion) noexcept override;
    int request_stop() noexcept override;
    int wait() noexcept override;
    int reset() noexcept override;

  private:
    WarpSpecializedQueue queue_;
    bool device_initialized_ = false;
};

class PersistentCopyClock {
  public:
    virtual ~PersistentCopyClock() = default;
    virtual std::uint64_t now_nanoseconds() noexcept = 0;
};

class SteadyPersistentCopyClock final : public PersistentCopyClock {
  public:
    std::uint64_t now_nanoseconds() noexcept override;
};

enum class TaskContextSlotState : std::uint32_t {
    free = 0,
    pending = 1,
    submitted = 2,
    local_error = 3,
};

enum class HostPublishFailure : std::uint32_t {
    none = 0,
    visibility = 1,
    queue = 2,
};

struct TaskContextSlot {
    std::uint64_t task_id = 0;
    std::uint64_t parent_request_id = 0;
    std::uint32_t payload_index = 0;
    TaskContextSlotState state = TaskContextSlotState::free;
    worker::DatagramResult result = worker::DatagramResult::success;
};

class PersistentCudaCopyHost {
  public:
    int initialize(const PersistentCudaCopyBackendConfig &config, PersistentCopyQueue *queue,
                   GpuDirectVisibilityGate *visibility_gate) noexcept;
    int try_submit(const worker::BackendRequest &request, std::uint64_t now_nanoseconds,
                   bool *accepted) noexcept;
    int progress_host_batch(std::uint64_t now_nanoseconds) noexcept;
    int publish_pending_now() noexcept;
    int try_pop_completion(worker::BackendCompletion *completion, std::uint64_t now_nanoseconds,
                           bool *completed) noexcept;
    int try_complete_locally(const worker::BackendRequest &request, worker::DatagramResult result,
                             bool *accepted) noexcept;
    int fail_pending(worker::DatagramResult result) noexcept;
    int fail_all(worker::DatagramResult result) noexcept;
    int reset() noexcept;

    [[nodiscard]] std::size_t outstanding_tasks() const noexcept;
    [[nodiscard]] std::size_t pending_tasks() const noexcept;
    [[nodiscard]] std::size_t local_completions() const noexcept;
    [[nodiscard]] HostPublishFailure last_publish_failure() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;

  private:
    int publish_pending() noexcept;

    PersistentCudaCopyBackendConfig config_{};
    PersistentCopyQueue *queue_ = nullptr;
    GpuDirectVisibilityGate *visibility_gate_ = nullptr;
    std::unique_ptr<TaskContextSlot[]> contexts_;
    std::unique_ptr<std::uint64_t[]> local_completion_task_ids_;
    std::array<CopyTask, kPersistentCudaCopyBackendHostBatch> pending_tasks_{};
    std::size_t capacity_mask_ = 0;
    std::size_t outstanding_tasks_ = 0;
    std::size_t pending_count_ = 0;
    std::uint64_t next_task_id_ = 1;
    std::uint64_t local_completion_head_ = 0;
    std::uint64_t local_completion_tail_ = 0;
    std::uint64_t first_pending_nanoseconds_ = 0;
    bool publish_blocked_ = false;
    bool pending_flushed_ = false;
    HostPublishFailure last_publish_failure_ = HostPublishFailure::none;
};

enum class PersistentCudaCopyBackendState : std::uint32_t {
    stopped = 0,
    accepting = 1,
    draining = 2,
};

class PersistentCudaCopyBackend final : public worker::CopyBackend {
  public:
    PersistentCudaCopyBackend() noexcept;
    PersistentCudaCopyBackend(PersistentCopyRuntime *runtime,
                              GpuDirectVisibilityGate *visibility_gate,
                              PersistentCopyClock *clock) noexcept;
    ~PersistentCudaCopyBackend() override;

    PersistentCudaCopyBackend(const PersistentCudaCopyBackend &) = delete;
    PersistentCudaCopyBackend &operator=(const PersistentCudaCopyBackend &) = delete;

    int start(const PersistentCudaCopyBackendConfig &config) noexcept;
    bool try_submit(const worker::BackendRequest &request) noexcept override;
    bool try_pop_completion(worker::BackendCompletion &completion) noexcept override;
    int request_stop() noexcept;
    int wait() noexcept;

    [[nodiscard]] PersistentCudaCopyBackendState state() const noexcept;
    [[nodiscard]] std::size_t outstanding_tasks() const noexcept;
    [[nodiscard]] int last_error() const noexcept;

  private:
    void handle_host_error(int status) noexcept;
    int progress_stop_request() noexcept;
    void abort_and_reset() noexcept;

    CudaPersistentCopyRuntime owned_runtime_;
    CudaGpuDirectVisibilityGate owned_visibility_gate_;
    SteadyPersistentCopyClock owned_clock_;
    PersistentCopyRuntime *runtime_ = nullptr;
    GpuDirectVisibilityGate *visibility_gate_ = nullptr;
    PersistentCopyClock *clock_ = nullptr;
    PersistentCudaCopyHost host_;
    PersistentCudaCopyBackendConfig config_{};
    PersistentCudaCopyBackendState state_ = PersistentCudaCopyBackendState::stopped;
    int last_error_ = 0;
    bool runtime_stop_requested_ = false;
};

}  // namespace ugdr::gpu

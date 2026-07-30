#pragma once

#include "gpu/persistent_copy.hpp"
#include "worker/copy_backend.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace ugdr::gpu {

constexpr std::uint32_t kPersistentCudaCopyBackendCopyWarps = 30;
constexpr std::uint32_t kPersistentCudaCopyBackendDeviceBatch = 16;
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

enum class TaskContextSlotState : std::uint32_t {
    free = 0,
    pending = 1,
    submitted = 2,
};

struct TaskContextSlot {
    std::uint64_t task_id = 0;
    std::uint64_t parent_request_id = 0;
    std::uint32_t payload_index = 0;
    TaskContextSlotState state = TaskContextSlotState::free;
};

class PersistentCudaCopyHost {
  public:
    int initialize(const PersistentCudaCopyBackendConfig &config,
                   PersistentCopyQueue *queue) noexcept;
    int try_submit(const worker::BackendRequest &request, std::uint64_t now_nanoseconds,
                   bool *accepted) noexcept;
    int progress_host_batch(std::uint64_t now_nanoseconds) noexcept;
    int try_pop_completion(worker::BackendCompletion *completion, std::uint64_t now_nanoseconds,
                           bool *completed) noexcept;

    [[nodiscard]] std::size_t outstanding_tasks() const noexcept;
    [[nodiscard]] std::size_t pending_tasks() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;

  private:
    int publish_pending() noexcept;

    PersistentCudaCopyBackendConfig config_{};
    PersistentCopyQueue *queue_ = nullptr;
    std::unique_ptr<TaskContextSlot[]> contexts_;
    std::array<CopyTask, kPersistentCudaCopyBackendHostBatch> pending_tasks_{};
    std::size_t capacity_mask_ = 0;
    std::size_t outstanding_tasks_ = 0;
    std::size_t pending_count_ = 0;
    std::uint64_t next_task_id_ = 1;
    std::uint64_t first_pending_nanoseconds_ = 0;
    bool publish_blocked_ = false;
};

}  // namespace ugdr::gpu

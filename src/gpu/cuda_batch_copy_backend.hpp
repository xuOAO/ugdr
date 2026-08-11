#pragma once

#include "worker/copy_backend.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ugdr::gpu {

constexpr std::size_t kCudaBatchCopyBackendMaxBatch = 64;
constexpr std::size_t kCudaBatchCopyBackendSlotCount = 4;
constexpr std::size_t kCudaBatchCopyBackendPendingCapacity =
    kCudaBatchCopyBackendMaxBatch * kCudaBatchCopyBackendSlotCount;
constexpr std::size_t kCudaBatchCopyBackendOutstandingCapacity =
    kCudaBatchCopyBackendPendingCapacity * 2;

struct CudaBatchCopyBackendConfig {
    std::uint32_t device_ordinal = 0;
    std::size_t max_batch = kCudaBatchCopyBackendMaxBatch;
    std::size_t slot_count = kCudaBatchCopyBackendSlotCount;
};

int validate_cuda_batch_copy_backend_config(const CudaBatchCopyBackendConfig &config) noexcept;

enum class CudaBatchSourceAccessOrder : std::uint32_t {
    stream = 1,
};

struct CudaBatchCopyAttributes {
    CudaBatchSourceAccessOrder source_access_order = CudaBatchSourceAccessOrder::stream;
    std::uint32_t flags = 0;
};

using CudaBatchStreamHandle = void *;
using CudaBatchEventHandle = void *;

class CudaBatchCopyRuntime {
  public:
    virtual ~CudaBatchCopyRuntime() = default;

    virtual int initialize_device(std::uint32_t device_ordinal) noexcept = 0;
    virtual int create_stream(CudaBatchStreamHandle *stream) noexcept = 0;
    virtual int destroy_stream(CudaBatchStreamHandle stream) noexcept = 0;
    virtual int create_event(CudaBatchEventHandle *event) noexcept = 0;
    virtual int destroy_event(CudaBatchEventHandle event) noexcept = 0;
    virtual int memcpy_batch_async(void **destinations, void **sources, std::size_t *sizes,
                                   std::size_t count, CudaBatchCopyAttributes *attributes,
                                   std::size_t *attribute_indices, std::size_t attribute_count,
                                   std::size_t *failed_index,
                                   CudaBatchStreamHandle stream) noexcept = 0;
    virtual int record_event(CudaBatchEventHandle event, CudaBatchStreamHandle stream) noexcept = 0;
    virtual int query_event(CudaBatchEventHandle event) noexcept = 0;
    virtual int synchronize_stream(CudaBatchStreamHandle stream) noexcept = 0;
};

class CudaBatchCopyRuntimeImpl final : public CudaBatchCopyRuntime {
  public:
    int initialize_device(std::uint32_t device_ordinal) noexcept override;
    int create_stream(CudaBatchStreamHandle *stream) noexcept override;
    int destroy_stream(CudaBatchStreamHandle stream) noexcept override;
    int create_event(CudaBatchEventHandle *event) noexcept override;
    int destroy_event(CudaBatchEventHandle event) noexcept override;
    int memcpy_batch_async(void **destinations, void **sources, std::size_t *sizes,
                           std::size_t count, CudaBatchCopyAttributes *attributes,
                           std::size_t *attribute_indices, std::size_t attribute_count,
                           std::size_t *failed_index,
                           CudaBatchStreamHandle stream) noexcept override;
    int record_event(CudaBatchEventHandle event, CudaBatchStreamHandle stream) noexcept override;
    int query_event(CudaBatchEventHandle event) noexcept override;
    int synchronize_stream(CudaBatchStreamHandle stream) noexcept override;
};

enum class CudaBatchSlotState : std::uint32_t {
    free = 0,
    preparing = 1,
    enqueued = 2,
    completed = 3,
    failed = 4,
};

enum class CudaBatchCopyBackendState : std::uint32_t {
    stopped = 0,
    accepting = 1,
    draining = 2,
};

struct CudaBatchSlot {
    std::array<void *, kCudaBatchCopyBackendMaxBatch> destinations{};
    std::array<void *, kCudaBatchCopyBackendMaxBatch> sources{};
    std::array<std::size_t, kCudaBatchCopyBackendMaxBatch> sizes{};
    std::array<worker::BackendRequest, kCudaBatchCopyBackendMaxBatch> requests{};
    CudaBatchCopyAttributes attributes{};
    std::size_t attribute_index = 0;
    std::size_t count = 0;
    std::uint64_t sequence = 0;
    CudaBatchEventHandle event = nullptr;
    CudaBatchSlotState state = CudaBatchSlotState::free;
};

class CudaBatchCopyBackend final : public worker::CopyBackend {
  public:
    CudaBatchCopyBackend() noexcept;
    explicit CudaBatchCopyBackend(CudaBatchCopyRuntime *runtime) noexcept;
    ~CudaBatchCopyBackend() override;

    CudaBatchCopyBackend(const CudaBatchCopyBackend &) = delete;
    CudaBatchCopyBackend &operator=(const CudaBatchCopyBackend &) = delete;

    int start(const CudaBatchCopyBackendConfig &config = {}) noexcept;
    bool try_submit(const worker::BackendRequest &request) noexcept override;
    std::size_t try_submit_batch(const worker::BackendRequest *requests,
                                 std::size_t request_count) noexcept override;
    bool flush_submissions() noexcept override;
    bool try_pop_completion(worker::BackendCompletion &completion) noexcept override;
    std::size_t try_pop_completion_batch(worker::BackendCompletion *completions,
                                         std::size_t completion_capacity) noexcept override;
    int request_stop() noexcept;
    int wait() noexcept;

    [[nodiscard]] CudaBatchCopyBackendState state() const noexcept;
    [[nodiscard]] std::size_t pending_tasks() const noexcept;
    [[nodiscard]] std::size_t in_flight_batches() const noexcept;
    [[nodiscard]] std::size_t outstanding_tasks() const noexcept;
    [[nodiscard]] std::uint64_t submitted_batches() const noexcept;
    [[nodiscard]] std::uint64_t submitted_payloads() const noexcept;
    [[nodiscard]] std::uint64_t event_queries() const noexcept;
    [[nodiscard]] std::uint64_t event_not_ready_queries() const noexcept;
    [[nodiscard]] int last_error() const noexcept;

  private:
    int progress() noexcept;
    int submit_one_batch(CudaBatchSlot &slot) noexcept;
    int fail_one_pending_batch(CudaBatchSlot &slot) noexcept;
    int poll_completed_prefix_newest_to_oldest() noexcept;
    int publish_terminal_prefix() noexcept;
    int enter_runtime_error(int status) noexcept;
    int synchronize_failed_stream() noexcept;
    int destroy_resources() noexcept;
    CudaBatchSlot *find_free_slot() noexcept;
    CudaBatchSlot *find_slot_by_sequence(std::uint64_t sequence) noexcept;
    const worker::BackendRequest &pending_at(std::size_t offset) const noexcept;
    void pop_pending(std::size_t count) noexcept;
    bool enqueue_completion(const worker::BackendRequest &request,
                            worker::DatagramResult result) noexcept;

    CudaBatchCopyRuntimeImpl owned_runtime_;
    CudaBatchCopyRuntime *runtime_ = nullptr;
    CudaBatchCopyBackendConfig config_{};
    CudaBatchStreamHandle stream_ = nullptr;
    std::array<CudaBatchSlot, kCudaBatchCopyBackendSlotCount> slots_{};
    std::array<worker::BackendRequest, kCudaBatchCopyBackendPendingCapacity> pending_{};
    std::array<worker::BackendCompletion, kCudaBatchCopyBackendOutstandingCapacity> completions_{};
    std::size_t pending_head_ = 0;
    std::size_t pending_count_ = 0;
    std::size_t completion_head_ = 0;
    std::size_t completion_count_ = 0;
    std::size_t in_flight_count_ = 0;
    std::size_t outstanding_count_ = 0;
    std::uint64_t oldest_sequence_ = 1;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t submitted_batches_ = 0;
    std::uint64_t submitted_payloads_ = 0;
    std::uint64_t event_queries_ = 0;
    std::uint64_t event_not_ready_queries_ = 0;
    CudaBatchCopyBackendState state_ = CudaBatchCopyBackendState::stopped;
    int last_error_ = 0;
};

}  // namespace ugdr::gpu

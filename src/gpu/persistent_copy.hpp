#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ugdr::gpu {

constexpr std::uint32_t kPersistentCopyResultSchemaVersion = 1;
constexpr std::size_t kPersistentCopyMaxPayloadBytes = 8192;
constexpr std::uint64_t kPersistentCopyMaxStageBufferBytes = UINT64_C(1) << 32;

enum class PersistentCopyModel : std::uint32_t {
    direct_atomic = 0,
    dynamic_sharded_spsc = 1,
    static_partition_spsc = 2,
    warp_specialized = 3,
};

const char *persistent_copy_model_name(PersistentCopyModel model) noexcept;
int parse_persistent_copy_model(const char *name, PersistentCopyModel *model) noexcept;

enum class CopyTaskResult : std::uint32_t {
    success = 0,
    invalid_task = 1,
    copy_failed = 2,
};

struct alignas(16) CopyTask {
    std::uint64_t task_id = 0;
    std::uint64_t target_address = 0;
    std::uint32_t length = 0;
    std::uint32_t relative_offset = 0;
};

struct alignas(16) CopyCompletion {
    std::uint64_t task_id = 0;
    CopyTaskResult result = CopyTaskResult::success;
};

struct alignas(16) CopyAccessCounts {
    std::uint64_t copied_bytes = 0;
    std::uint64_t vector_128_bytes = 0;
    std::uint64_t narrow_bytes = 0;
};

static_assert(std::is_standard_layout_v<CopyTask>);
static_assert(std::is_trivially_copyable_v<CopyTask>);
static_assert(std::is_standard_layout_v<CopyCompletion>);
static_assert(std::is_trivially_copyable_v<CopyCompletion>);
static_assert(std::is_standard_layout_v<CopyAccessCounts>);
static_assert(std::is_trivially_copyable_v<CopyAccessCounts>);
static_assert(sizeof(CopyTask) == 32);
static_assert(sizeof(CopyCompletion) == 16);
static_assert(offsetof(CopyTask, task_id) == 0);
static_assert(offsetof(CopyTask, target_address) == 8);
static_assert(offsetof(CopyTask, length) == 16);
static_assert(offsetof(CopyTask, relative_offset) == 20);
static_assert(offsetof(CopyCompletion, task_id) == 0);
static_assert(offsetof(CopyCompletion, result) == 8);

struct PersistentCopyConfig {
    PersistentCopyModel model = PersistentCopyModel::direct_atomic;
    std::uint32_t device_ordinal = 0;
    std::uint32_t copy_warps = 4;
    std::uint32_t shared_stage_count = 0;
    std::size_t payload_bytes = kPersistentCopyMaxPayloadBytes;
    std::size_t parent_wr_bytes = 64 * 1024;
    std::size_t outstanding_capacity = 1024;
    std::size_t host_batch = 32;
    std::uint64_t warmup_tasks = 1000;
    std::uint64_t iterations = 10000;
};

int validate_persistent_copy_config(const PersistentCopyConfig &config) noexcept;
int initialize_persistent_copy_device(std::uint32_t device_ordinal) noexcept;
int launch_persistent_copy_core_test(std::uint64_t stage_buffer_base, const CopyTask &task,
                                     std::uint64_t access_counts_address) noexcept;

struct PersistentCopyResult {
    std::uint32_t schema_version = kPersistentCopyResultSchemaVersion;
    PersistentCopyModel model = PersistentCopyModel::direct_atomic;
    std::size_t payload_bytes = 0;
    std::size_t parent_wr_bytes = 0;
    std::size_t outstanding_capacity = 0;
    std::size_t host_batch = 0;
    std::uint32_t copy_warps = 0;
    std::uint32_t cta_count = 0;
    std::uint32_t ring_count = 0;
    bool host_warp_aware = false;
    std::size_t host_meta_bytes = 0;
    std::uint64_t host_system_atomic_operations = 0;
    std::size_t dynamic_shared_memory_bytes = 0;
    std::uint32_t registers_per_thread = 0;
    double occupancy = 0.0;
    std::uint64_t accepted_tasks = 0;
    std::uint64_t completed_tasks = 0;
    std::uint64_t drained_tasks = 0;
    std::uint64_t copied_bytes = 0;
    double elapsed_seconds = 0.0;
    double task_millions_per_second = 0.0;
    double copy_gigabytes_per_second = 0.0;
    double task_p50_microseconds = 0.0;
    double task_p99_microseconds = 0.0;
    double host_cpu_percent = 0.0;
    bool correctness_passed = false;
    bool measurement_valid = false;
};

class MappedPinnedMemory {
  public:
    MappedPinnedMemory() noexcept = default;
    ~MappedPinnedMemory();

    MappedPinnedMemory(const MappedPinnedMemory &) = delete;
    MappedPinnedMemory &operator=(const MappedPinnedMemory &) = delete;
    MappedPinnedMemory(MappedPinnedMemory &&other) noexcept;
    MappedPinnedMemory &operator=(MappedPinnedMemory &&other) noexcept;

    static int allocate(std::size_t bytes, MappedPinnedMemory *memory) noexcept;
    int reset() noexcept;

    [[nodiscard]] void *host_data() noexcept;
    [[nodiscard]] const void *host_data() const noexcept;
    [[nodiscard]] std::uint64_t device_address() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

  private:
    void *host_data_ = nullptr;
    void *device_data_ = nullptr;
    std::size_t size_ = 0;
};

struct PayloadCheck {
    bool payload_matches = false;
    bool guards_intact = false;
    std::size_t mismatch_count = 0;
    std::size_t first_mismatch = 0;
};

std::uint8_t persistent_copy_payload_byte(std::uint64_t seed, std::size_t index) noexcept;

class PersistentCopyPayloadBuffer {
  public:
    PersistentCopyPayloadBuffer() noexcept = default;
    ~PersistentCopyPayloadBuffer();

    PersistentCopyPayloadBuffer(const PersistentCopyPayloadBuffer &) = delete;
    PersistentCopyPayloadBuffer &operator=(const PersistentCopyPayloadBuffer &) = delete;
    PersistentCopyPayloadBuffer(PersistentCopyPayloadBuffer &&other) noexcept;
    PersistentCopyPayloadBuffer &operator=(PersistentCopyPayloadBuffer &&other) noexcept;

    static int allocate(std::size_t payload_capacity, std::size_t guard_bytes,
                        PersistentCopyPayloadBuffer *buffer) noexcept;
    int prepare(std::uint64_t seed) noexcept;
    int verify(std::uint64_t seed, PayloadCheck *check) const noexcept;
    int verify_copy(std::uint64_t seed, std::size_t source_offset, std::size_t target_offset,
                    std::size_t length, PayloadCheck *check) const noexcept;
    int make_task(std::uint64_t task_id, std::uint64_t target_address, std::size_t length,
                  std::uint32_t relative_offset, CopyTask *task) const noexcept;
    int reset() noexcept;

    [[nodiscard]] std::uint64_t stage_buffer_base() const noexcept;
    [[nodiscard]] std::uint64_t target_address() const noexcept;
    [[nodiscard]] std::size_t payload_capacity() const noexcept;
    [[nodiscard]] std::size_t guard_bytes() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

  private:
    void *stage_buffer_allocation_ = nullptr;
    void *target_allocation_ = nullptr;
    std::size_t payload_capacity_ = 0;
    std::size_t guard_bytes_ = 0;
};

class DirectAtomicQueue {
  public:
    DirectAtomicQueue() noexcept = default;
    ~DirectAtomicQueue();

    DirectAtomicQueue(const DirectAtomicQueue &) = delete;
    DirectAtomicQueue &operator=(const DirectAtomicQueue &) = delete;

    static int allocate(std::size_t capacity, std::uint32_t copy_warps,
                        std::uint64_t stage_buffer_base, DirectAtomicQueue *queue) noexcept;
    int start() noexcept;
    int try_submit(const CopyTask &task) noexcept;
    int try_poll(CopyCompletion *completion) noexcept;
    int request_stop() noexcept;
    int wait() noexcept;
    int reset() noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t host_meta_bytes() const noexcept;
    [[nodiscard]] std::uint32_t copy_warps() const noexcept;
    [[nodiscard]] std::uint64_t accepted_tasks() const noexcept;
    [[nodiscard]] std::uint64_t completed_tasks() const noexcept;
    [[nodiscard]] std::uint64_t host_system_atomic_operations() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool drained() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

  private:
    MappedPinnedMemory memory_;
    std::size_t capacity_ = 0;
    std::uint32_t copy_warps_ = 0;
    std::uint64_t stage_buffer_base_ = 0;
    std::uint64_t submit_tail_ = 0;
    std::uint64_t completion_head_ = 0;
    bool running_ = false;
    bool accepting_ = false;
};

enum class PersistentCopyLifecycleState : std::uint32_t {
    stopped = 0,
    accepting = 1,
    draining = 2,
};

class PersistentCopyLifecycle {
  public:
    int start(const PersistentCopyConfig &config) noexcept;
    int record_accepted(std::uint64_t count = 1) noexcept;
    int record_completed(std::uint64_t count = 1) noexcept;
    int request_stop() noexcept;
    int finish_stop() noexcept;

    [[nodiscard]] PersistentCopyLifecycleState state() const noexcept;
    [[nodiscard]] const PersistentCopyConfig &config() const noexcept;
    [[nodiscard]] std::uint64_t accepted_tasks() const noexcept;
    [[nodiscard]] std::uint64_t completed_tasks() const noexcept;
    [[nodiscard]] bool drained() const noexcept;

  private:
    PersistentCopyConfig config_{};
    PersistentCopyLifecycleState state_ = PersistentCopyLifecycleState::stopped;
    std::uint64_t accepted_tasks_ = 0;
    std::uint64_t completed_tasks_ = 0;
};

}  // namespace ugdr::gpu

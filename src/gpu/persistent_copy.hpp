#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ugdr::gpu {

constexpr std::uint32_t kPersistentCopyResultSchemaVersion = 1;
constexpr std::size_t kPersistentCopyMaxPayloadBytes = 8192;

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
    std::uint64_t parent_request_id = 0;
    std::uint64_t source_address = 0;
    std::uint64_t target_address = 0;
    std::uint32_t payload_index = 0;
    std::uint32_t length = 0;
};

struct alignas(16) CopyCompletion {
    std::uint64_t task_id = 0;
    std::uint64_t parent_request_id = 0;
    std::uint32_t payload_index = 0;
    CopyTaskResult result = CopyTaskResult::success;
};

static_assert(std::is_standard_layout_v<CopyTask>);
static_assert(std::is_trivially_copyable_v<CopyTask>);
static_assert(std::is_standard_layout_v<CopyCompletion>);
static_assert(std::is_trivially_copyable_v<CopyCompletion>);

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
    int make_task(std::uint64_t task_id, std::uint64_t parent_request_id,
                  std::uint32_t payload_index, std::size_t length, CopyTask *task) const noexcept;
    int reset() noexcept;

    [[nodiscard]] std::uint64_t source_address() const noexcept;
    [[nodiscard]] std::uint64_t target_address() const noexcept;
    [[nodiscard]] std::size_t payload_capacity() const noexcept;
    [[nodiscard]] std::size_t guard_bytes() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

  private:
    void *source_allocation_ = nullptr;
    void *target_allocation_ = nullptr;
    std::size_t payload_capacity_ = 0;
    std::size_t guard_bytes_ = 0;
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

#include "gpu/persistent_copy.hpp"
#include "gpu/persistent_copy_backend.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

namespace {

using ugdr::gpu::CopyCompletion;
using ugdr::gpu::CopyTask;
using ugdr::gpu::CopyTaskResult;
using ugdr::gpu::GpuDirectVisibilityGate;
using ugdr::gpu::PersistentCopyQueue;
using ugdr::worker::BackendCompletion;
using ugdr::worker::BackendRequest;
using ugdr::worker::DatagramResult;

class FakePersistentCopyQueue final : public PersistentCopyQueue {
  public:
    int try_submit_batch(const CopyTask *tasks, std::size_t task_count,
                         std::size_t *submitted_count) noexcept override {
        ++submit_calls;
        last_requested = task_count;
        if (events != nullptr) {
            events->push_back(2);
        }
        if (submit_status != 0) {
            return submit_status;
        }
        const std::size_t count = task_count < submit_limit ? task_count : submit_limit;
        submitted.insert(submitted.end(), tasks, tasks + count);
        *submitted_count = count;
        return 0;
    }

    int try_poll(CopyCompletion *completion) noexcept override {
        if (completions.empty()) {
            return EAGAIN;
        }
        *completion = completions.front();
        completions.pop_front();
        return 0;
    }

    std::size_t submit_limit = std::numeric_limits<std::size_t>::max();
    int submit_status = 0;
    std::size_t submit_calls = 0;
    std::size_t last_requested = 0;
    std::vector<int> *events = nullptr;
    std::vector<CopyTask> submitted;
    std::deque<CopyCompletion> completions;
};

class FakeVisibilityGate final : public GpuDirectVisibilityGate {
  public:
    int initialize(std::uint32_t device_ordinal) noexcept override {
        initialized_device = device_ordinal;
        return initialize_status;
    }

    int flush_current_context_to_owner() noexcept override {
        ++flush_calls;
        if (events != nullptr) {
            events->push_back(1);
        }
        return flush_status;
    }

    std::uint32_t initialized_device = 0;
    int initialize_status = 0;
    int flush_status = 0;
    std::size_t flush_calls = 0;
    std::vector<int> *events = nullptr;
};

ugdr::gpu::PersistentCudaCopyBackendConfig valid_config() {
    ugdr::gpu::PersistentCudaCopyBackendConfig config;
    config.stage_buffer_base = UINT64_C(0x100000);
    config.stage_buffer_bytes = 64 * 1024;
    config.queue_capacity = 1024;
    config.host_batch = ugdr::gpu::kPersistentCudaCopyBackendHostBatch;
    config.max_batch_delay_nanoseconds = 50'000;
    return config;
}

BackendRequest valid_request(std::uint64_t index) {
    BackendRequest request;
    request.parent_request_id = 1000 + index;
    request.source_daemon_address = UINT64_C(0x100000) + index * 32;
    request.target_daemon_address = UINT64_C(0x200000) + index * 32;
    request.payload_length = 16;
    request.payload_index = static_cast<std::uint32_t>(index);
    return request;
}

int config_validation_test() {
    using ugdr::gpu::validate_persistent_cuda_copy_backend_config;

    auto config = valid_config();
    if (validate_persistent_cuda_copy_backend_config(config) != 0) {
        return 1;
    }
    config.stage_buffer_base = 0;
    if (validate_persistent_cuda_copy_backend_config(config) != EINVAL) {
        return 2;
    }
    config = valid_config();
    config.stage_buffer_bytes = 0;
    if (validate_persistent_cuda_copy_backend_config(config) != EINVAL) {
        return 3;
    }
    config = valid_config();
    config.stage_buffer_bytes = ugdr::gpu::kPersistentCopyMaxStageBufferBytes + 1;
    if (validate_persistent_cuda_copy_backend_config(config) != EINVAL) {
        return 4;
    }
    config = valid_config();
    config.stage_buffer_base = std::numeric_limits<std::uint64_t>::max() - 15;
    config.stage_buffer_bytes = 16;
    if (validate_persistent_cuda_copy_backend_config(config) != EINVAL) {
        return 5;
    }
    config = valid_config();
    config.queue_capacity = 1000;
    if (validate_persistent_cuda_copy_backend_config(config) != EINVAL) {
        return 6;
    }
    config = valid_config();
    config.queue_capacity = ugdr::gpu::kPersistentCudaCopyBackendHostBatch / 2;
    if (validate_persistent_cuda_copy_backend_config(config) != EINVAL) {
        return 7;
    }
    config = valid_config();
    config.host_batch = ugdr::gpu::kPersistentCudaCopyBackendHostBatch / 2;
    if (validate_persistent_cuda_copy_backend_config(config) != EINVAL) {
        return 8;
    }
    config = valid_config();
    config.max_batch_delay_nanoseconds = 0;
    if (validate_persistent_cuda_copy_backend_config(config) != EINVAL) {
        return 9;
    }
    return 0;
}

int timed_batch_and_completion_test() {
    auto config = valid_config();
    config.queue_capacity = 128;
    config.max_batch_delay_nanoseconds = 100;
    FakePersistentCopyQueue queue;
    FakeVisibilityGate visibility;
    ugdr::gpu::PersistentCudaCopyHost host;
    if (host.initialize(config, &queue, &visibility) != 0 || !host.initialized()) {
        return 1;
    }

    for (std::uint64_t index = 0; index < 63; ++index) {
        bool accepted = false;
        if (host.try_submit(valid_request(index), 100, &accepted) != 0 || !accepted) {
            return 2;
        }
    }
    if (host.pending_tasks() != 63 || host.outstanding_tasks() != 63 || !queue.submitted.empty() ||
        host.progress_host_batch(199) != 0 || queue.submit_calls != 0 ||
        host.progress_host_batch(200) != 0 || queue.submitted.size() != 63 ||
        host.pending_tasks() != 0) {
        return 3;
    }
    if (queue.submitted.front().task_id != 1 || queue.submitted.front().relative_offset != 0 ||
        queue.submitted.back().task_id != 63 || queue.submitted.back().relative_offset != 62 * 32) {
        return 4;
    }

    queue.completions.push_back({queue.submitted[0].task_id, CopyTaskResult::success});
    queue.completions.push_back({queue.submitted[1].task_id, CopyTaskResult::copy_failed});
    BackendCompletion completion{};
    bool completed = false;
    if (host.try_pop_completion(&completion, 200, &completed) != 0 || !completed ||
        completion.parent_request_id != 1000 || completion.payload_index != 0 ||
        completion.result != DatagramResult::success) {
        return 5;
    }
    if (host.try_pop_completion(&completion, 200, &completed) != 0 || !completed ||
        completion.parent_request_id != 1001 || completion.payload_index != 1 ||
        completion.result != DatagramResult::backend_error || host.outstanding_tasks() != 61) {
        return 6;
    }
    if (host.try_pop_completion(&completion, 200, &completed) != 0 || completed) {
        return 7;
    }
    return 0;
}

int full_and_partial_batch_test() {
    auto config = valid_config();
    config.queue_capacity = 64;
    config.max_batch_delay_nanoseconds = 100;
    FakePersistentCopyQueue queue;
    queue.submit_limit = 10;
    FakeVisibilityGate visibility;
    ugdr::gpu::PersistentCudaCopyHost host;
    if (host.initialize(config, &queue, &visibility) != 0) {
        return 1;
    }

    for (std::uint64_t index = 0; index < 64; ++index) {
        bool accepted = false;
        if (host.try_submit(valid_request(index), 100, &accepted) != 0 || !accepted) {
            return 2;
        }
    }
    if (queue.submit_calls != 1 ||
        queue.last_requested != ugdr::gpu::kPersistentCudaCopyBackendHostBatch ||
        queue.submitted.size() != 10 || host.pending_tasks() != 54 ||
        host.outstanding_tasks() != 64) {
        return 3;
    }
    bool accepted = true;
    if (host.try_submit(valid_request(64), 100, &accepted) != 0 || accepted) {
        return 4;
    }
    if (host.progress_host_batch(100) != 0 || visibility.flush_calls != 1 ||
        queue.submit_calls != 2 || queue.submitted.size() != 20 || host.pending_tasks() != 44 ||
        queue.submitted[10].task_id != 11 || queue.submitted[19].task_id != 20) {
        return 5;
    }
    return 0;
}

int invalid_request_and_completion_test() {
    auto config = valid_config();
    config.queue_capacity = 64;
    FakePersistentCopyQueue queue;
    FakeVisibilityGate visibility;
    ugdr::gpu::PersistentCudaCopyHost host;
    if (host.initialize(config, &queue, &visibility) != 0) {
        return 1;
    }

    auto request = valid_request(0);
    request.source_daemon_address = config.stage_buffer_base + config.stage_buffer_bytes - 8;
    request.payload_length = 16;
    bool accepted = true;
    if (host.try_submit(request, 0, &accepted) != EINVAL || accepted ||
        host.outstanding_tasks() != 0) {
        return 2;
    }
    queue.completions.push_back({999, CopyTaskResult::success});
    BackendCompletion completion{};
    bool completed = true;
    if (host.try_pop_completion(&completion, 0, &completed) != EPROTO || completed) {
        return 3;
    }
    return 0;
}

int context_ring_reuse_test() {
    auto config = valid_config();
    config.queue_capacity = 64;
    config.max_batch_delay_nanoseconds = 100;
    FakePersistentCopyQueue queue;
    FakeVisibilityGate visibility;
    ugdr::gpu::PersistentCudaCopyHost host;
    if (host.initialize(config, &queue, &visibility) != 0) {
        return 1;
    }
    for (std::uint64_t index = 0; index < 64; ++index) {
        bool accepted = false;
        if (host.try_submit(valid_request(index), 100, &accepted) != 0 || !accepted) {
            return 2;
        }
    }

    queue.completions.push_back({65, CopyTaskResult::success});
    BackendCompletion completion{};
    bool completed = true;
    if (host.try_pop_completion(&completion, 100, &completed) != EPROTO || completed) {
        return 3;
    }
    queue.completions.push_back({1, CopyTaskResult::success});
    if (host.try_pop_completion(&completion, 100, &completed) != 0 || !completed) {
        return 4;
    }
    bool accepted = false;
    if (host.try_submit(valid_request(64), 100, &accepted) != 0 || !accepted ||
        host.outstanding_tasks() != 64 || host.progress_host_batch(200) != 0 ||
        queue.submitted.back().task_id != 65) {
        return 5;
    }
    return 0;
}

int visibility_order_and_failure_test() {
    auto config = valid_config();
    config.queue_capacity = 64;
    config.max_batch_delay_nanoseconds = 100;
    std::vector<int> events;
    FakePersistentCopyQueue queue;
    queue.events = &events;
    FakeVisibilityGate visibility;
    visibility.events = &events;
    visibility.flush_status = EIO;
    ugdr::gpu::PersistentCudaCopyHost host;
    if (host.initialize(config, &queue, &visibility) != 0) {
        return 1;
    }
    for (std::uint64_t index = 0; index < 64; ++index) {
        bool accepted = false;
        const int status = host.try_submit(valid_request(index), 100, &accepted);
        if ((index != 63 && status != 0) || (index == 63 && status != EIO) || !accepted) {
            return 2;
        }
    }
    if (events != std::vector<int>{1} || visibility.flush_calls != 1 || queue.submit_calls != 0 ||
        host.pending_tasks() != 64) {
        return 3;
    }

    visibility.flush_status = 0;
    if (host.progress_host_batch(100) != 0 || events != std::vector<int>({1, 1, 2}) ||
        visibility.flush_calls != 2 || queue.submit_calls != 1 || host.pending_tasks() != 0) {
        return 4;
    }
    return 0;
}

}  // namespace

int main() {
    const int config_status = config_validation_test();
    if (config_status != 0) {
        return 10 + config_status;
    }
    const int timed_status = timed_batch_and_completion_test();
    if (timed_status != 0) {
        return 30 + timed_status;
    }
    const int partial_status = full_and_partial_batch_test();
    if (partial_status != 0) {
        return 50 + partial_status;
    }
    const int invalid_status = invalid_request_and_completion_test();
    if (invalid_status != 0) {
        return 70 + invalid_status;
    }
    const int reuse_status = context_ring_reuse_test();
    if (reuse_status != 0) {
        return 90 + reuse_status;
    }
    const int visibility_status = visibility_order_and_failure_test();
    if (visibility_status != 0) {
        return 110 + visibility_status;
    }
    return 0;
}

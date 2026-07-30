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
using ugdr::gpu::PersistentCopyClock;
using ugdr::gpu::PersistentCopyQueue;
using ugdr::gpu::PersistentCopyRuntime;
using ugdr::worker::BackendCompletion;
using ugdr::worker::BackendRequest;
using ugdr::worker::DatagramResult;

class FakePersistentCopyQueue final : public PersistentCopyRuntime {
  public:
    int initialize_device(std::uint32_t device_ordinal) noexcept override {
        ++initialize_device_calls;
        initialized_device = device_ordinal;
        return initialize_device_status;
    }

    int start(const ugdr::gpu::PersistentCudaCopyBackendConfig &) noexcept override {
        ++start_calls;
        return start_status;
    }

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

    int request_stop() noexcept override {
        ++request_stop_calls;
        return request_stop_status;
    }

    int wait() noexcept override {
        ++wait_calls;
        return wait_status;
    }

    int reset() noexcept override {
        ++reset_calls;
        return reset_status;
    }

    std::uint32_t initialized_device = 0;
    int initialize_device_status = 0;
    int start_status = 0;
    std::size_t submit_limit = std::numeric_limits<std::size_t>::max();
    int submit_status = 0;
    int request_stop_status = 0;
    int wait_status = 0;
    int reset_status = 0;
    std::size_t initialize_device_calls = 0;
    std::size_t start_calls = 0;
    std::size_t submit_calls = 0;
    std::size_t request_stop_calls = 0;
    std::size_t wait_calls = 0;
    std::size_t reset_calls = 0;
    std::size_t last_requested = 0;
    std::vector<int> *events = nullptr;
    std::vector<CopyTask> submitted;
    std::deque<CopyCompletion> completions;
};

class FakeVisibilityGate final : public GpuDirectVisibilityGate {
  public:
    int initialize(std::uint32_t device_ordinal) noexcept override {
        ++initialize_calls;
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

    int reset() noexcept override {
        ++reset_calls;
        return reset_status;
    }

    std::uint32_t initialized_device = 0;
    int initialize_status = 0;
    int flush_status = 0;
    int reset_status = 0;
    std::size_t initialize_calls = 0;
    std::size_t flush_calls = 0;
    std::size_t reset_calls = 0;
    std::vector<int> *events = nullptr;
};

class FakeClock final : public PersistentCopyClock {
  public:
    std::uint64_t now_nanoseconds() noexcept override {
        return now;
    }

    std::uint64_t now = 0;
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

int backend_lifecycle_test() {
    auto config = valid_config();
    config.queue_capacity = 64;
    config.max_batch_delay_nanoseconds = 100;
    FakePersistentCopyQueue runtime;
    FakeVisibilityGate visibility;
    FakeClock clock;
    clock.now = 100;
    ugdr::gpu::PersistentCudaCopyBackend backend(&runtime, &visibility, &clock);
    if (backend.start(config) != 0 ||
        backend.state() != ugdr::gpu::PersistentCudaCopyBackendState::accepting ||
        runtime.initialize_device_calls != 1 || runtime.start_calls != 1 ||
        visibility.initialize_calls != 1) {
        return 1;
    }

    auto invalid_request = valid_request(0);
    invalid_request.source_daemon_address = config.stage_buffer_base - 1;
    if (!backend.try_submit(invalid_request) || backend.outstanding_tasks() != 1) {
        return 2;
    }
    BackendCompletion completion{};
    if (!backend.try_pop_completion(completion) ||
        completion.parent_request_id != invalid_request.parent_request_id ||
        completion.result != DatagramResult::backend_error || backend.outstanding_tasks() != 0) {
        return 3;
    }

    const BackendRequest request = valid_request(0);
    if (!backend.try_submit(request) || backend.request_stop() != 0 ||
        backend.state() != ugdr::gpu::PersistentCudaCopyBackendState::draining ||
        runtime.submitted.size() != 1 || runtime.submitted.front().task_id != 2 ||
        runtime.request_stop_calls != 1 || backend.wait() != EAGAIN) {
        return 4;
    }
    runtime.completions.push_back({runtime.submitted.front().task_id, CopyTaskResult::success});
    if (!backend.try_pop_completion(completion) ||
        completion.parent_request_id != request.parent_request_id ||
        completion.result != DatagramResult::success || backend.outstanding_tasks() != 0 ||
        backend.wait() != 0 ||
        backend.state() != ugdr::gpu::PersistentCudaCopyBackendState::stopped ||
        runtime.wait_calls != 1 || runtime.reset_calls != 1 || visibility.reset_calls != 1) {
        return 5;
    }
    if (backend.request_stop() != EINVAL || backend.try_submit(request) ||
        backend.try_pop_completion(completion)) {
        return 6;
    }
    return 0;
}

int backend_error_closure_test() {
    auto config = valid_config();
    config.queue_capacity = 64;
    config.max_batch_delay_nanoseconds = 100;

    {
        FakePersistentCopyQueue runtime;
        FakeVisibilityGate visibility;
        FakeClock clock;
        clock.now = 100;
        ugdr::gpu::PersistentCudaCopyBackend backend(&runtime, &visibility, &clock);
        if (backend.start(config) != 0 || !backend.try_submit(valid_request(0))) {
            return 1;
        }
        visibility.flush_status = EIO;
        clock.now = 200;
        BackendCompletion completion{};
        if (!backend.try_pop_completion(completion) ||
            completion.result != DatagramResult::backend_error ||
            backend.state() != ugdr::gpu::PersistentCudaCopyBackendState::accepting ||
            backend.last_error() != EIO || runtime.submit_calls != 0 ||
            backend.request_stop() != 0 || backend.wait() != 0) {
            return 2;
        }
    }

    {
        FakePersistentCopyQueue runtime;
        FakeVisibilityGate visibility;
        FakeClock clock;
        clock.now = 100;
        ugdr::gpu::PersistentCudaCopyBackend backend(&runtime, &visibility, &clock);
        if (backend.start(config) != 0 || !backend.try_submit(valid_request(0))) {
            return 3;
        }
        runtime.submit_status = EIO;
        clock.now = 200;
        BackendCompletion completion{};
        if (!backend.try_pop_completion(completion) ||
            completion.result != DatagramResult::backend_error ||
            backend.state() != ugdr::gpu::PersistentCudaCopyBackendState::draining ||
            backend.last_error() != EIO || runtime.submit_calls != 1 ||
            runtime.request_stop_calls != 1 || backend.wait() != 0 ||
            backend.state() != ugdr::gpu::PersistentCudaCopyBackendState::stopped) {
            return 4;
        }
    }
    return 0;
}

int backend_start_failure_test() {
    auto config = valid_config();
    config.queue_capacity = 64;

    {
        FakePersistentCopyQueue runtime;
        FakeVisibilityGate visibility;
        visibility.initialize_status = EOPNOTSUPP;
        FakeClock clock;
        ugdr::gpu::PersistentCudaCopyBackend backend(&runtime, &visibility, &clock);
        if (backend.start(config) != EOPNOTSUPP ||
            backend.state() != ugdr::gpu::PersistentCudaCopyBackendState::stopped ||
            runtime.reset_calls != 1 || visibility.reset_calls != 1) {
            return 1;
        }
    }

    {
        FakePersistentCopyQueue runtime;
        runtime.start_status = EIO;
        FakeVisibilityGate visibility;
        FakeClock clock;
        ugdr::gpu::PersistentCudaCopyBackend backend(&runtime, &visibility, &clock);
        if (backend.start(config) != EIO ||
            backend.state() != ugdr::gpu::PersistentCudaCopyBackendState::stopped ||
            runtime.reset_calls != 1 || visibility.reset_calls != 1) {
            return 2;
        }
        runtime.start_status = 0;
        if (backend.start(config) != 0 || backend.request_stop() != 0 || backend.wait() != 0) {
            return 3;
        }
    }
    return 0;
}

int backend_backpressure_test() {
    auto config = valid_config();
    config.queue_capacity = 64;
    config.max_batch_delay_nanoseconds = 100;
    FakePersistentCopyQueue runtime;
    FakeVisibilityGate visibility;
    FakeClock clock;
    clock.now = 100;
    ugdr::gpu::PersistentCudaCopyBackend backend(&runtime, &visibility, &clock);
    if (backend.start(config) != 0) {
        return 1;
    }
    for (std::uint64_t index = 0; index < 64; ++index) {
        if (!backend.try_submit(valid_request(index))) {
            return 2;
        }
    }
    if (backend.try_submit(valid_request(64)) || backend.outstanding_tasks() != 64 ||
        runtime.submitted.size() != 64) {
        return 3;
    }

    runtime.completions.push_back({1, CopyTaskResult::success});
    BackendCompletion completion{};
    if (!backend.try_pop_completion(completion) || completion.parent_request_id != 1000 ||
        !backend.try_submit(valid_request(64)) || backend.outstanding_tasks() != 64 ||
        backend.request_stop() != 0 || runtime.submitted.size() != 65 ||
        runtime.submitted.back().task_id != 65) {
        return 4;
    }
    for (std::uint64_t task_id = 2; task_id <= 65; ++task_id) {
        runtime.completions.push_back({task_id, CopyTaskResult::success});
    }
    for (std::uint64_t index = 1; index < 65; ++index) {
        if (!backend.try_pop_completion(completion) ||
            completion.parent_request_id != 1000 + index ||
            completion.result != DatagramResult::success) {
            return 5;
        }
    }
    if (backend.outstanding_tasks() != 0 || backend.wait() != 0 || visibility.flush_calls != 2) {
        return 6;
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
    const int lifecycle_status = backend_lifecycle_test();
    if (lifecycle_status != 0) {
        return 130 + lifecycle_status;
    }
    const int closure_status = backend_error_closure_test();
    if (closure_status != 0) {
        return 150 + closure_status;
    }
    const int start_failure_status = backend_start_failure_test();
    if (start_failure_status != 0) {
        return 170 + start_failure_status;
    }
    const int backpressure_status = backend_backpressure_test();
    if (backpressure_status != 0) {
        return 190 + backpressure_status;
    }
    return 0;
}

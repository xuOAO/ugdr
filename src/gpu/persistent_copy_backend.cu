#include "gpu/persistent_copy_backend.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace ugdr::gpu {
namespace {

bool is_power_of_two(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

int make_copy_task(const PersistentCudaCopyBackendConfig &config,
                   const worker::BackendRequest &request, std::uint64_t task_id,
                   CopyTask *task) noexcept {
    if (task == nullptr || request.source_daemon_address < config.stage_buffer_base ||
        request.target_daemon_address == 0 || request.payload_length == 0 ||
        request.payload_length > kPersistentCopyMaxPayloadBytes) {
        return EINVAL;
    }
    const std::uint64_t relative_offset = request.source_daemon_address - config.stage_buffer_base;
    if (relative_offset >= config.stage_buffer_bytes ||
        request.payload_length > config.stage_buffer_bytes - relative_offset ||
        request.target_daemon_address >
            std::numeric_limits<std::uint64_t>::max() - request.payload_length ||
        relative_offset > std::numeric_limits<std::uint32_t>::max()) {
        return EINVAL;
    }
    task->task_id = task_id;
    task->target_address = request.target_daemon_address;
    task->length = request.payload_length;
    task->relative_offset = static_cast<std::uint32_t>(relative_offset);
    return 0;
}

}  // namespace

int validate_persistent_cuda_copy_backend_config(
    const PersistentCudaCopyBackendConfig &config) noexcept {
    if (config.stage_buffer_base == 0 || config.stage_buffer_bytes == 0 ||
        config.stage_buffer_bytes > kPersistentCopyMaxStageBufferBytes ||
        config.stage_buffer_base >
            std::numeric_limits<std::uint64_t>::max() - config.stage_buffer_bytes) {
        return EINVAL;
    }
    if (!is_power_of_two(config.queue_capacity) ||
        config.queue_capacity < kPersistentCudaCopyBackendHostBatch ||
        config.host_batch != kPersistentCudaCopyBackendHostBatch ||
        config.max_batch_delay_nanoseconds == 0) {
        return EINVAL;
    }
    return 0;
}

int PersistentCudaCopyHost::initialize(const PersistentCudaCopyBackendConfig &config,
                                       PersistentCopyQueue *queue,
                                       GpuDirectVisibilityGate *visibility_gate) noexcept {
    if (initialized() || queue == nullptr || visibility_gate == nullptr ||
        validate_persistent_cuda_copy_backend_config(config) != 0 ||
        config.queue_capacity > std::numeric_limits<std::size_t>::max() / sizeof(TaskContextSlot)) {
        return EINVAL;
    }
    std::unique_ptr<TaskContextSlot[]> contexts(new (std::nothrow)
                                                    TaskContextSlot[config.queue_capacity]);
    std::unique_ptr<std::uint64_t[]> local_completion_task_ids(
        new (std::nothrow) std::uint64_t[config.queue_capacity]);
    if (contexts == nullptr || local_completion_task_ids == nullptr) {
        return ENOMEM;
    }
    config_ = config;
    queue_ = queue;
    visibility_gate_ = visibility_gate;
    contexts_ = std::move(contexts);
    local_completion_task_ids_ = std::move(local_completion_task_ids);
    capacity_mask_ = config.queue_capacity - 1;
    return 0;
}

int PersistentCudaCopyHost::try_submit(const worker::BackendRequest &request,
                                       std::uint64_t now_nanoseconds, bool *accepted) noexcept {
    if (accepted == nullptr || !initialized()) {
        return EINVAL;
    }
    *accepted = false;
    if (outstanding_tasks_ >= config_.queue_capacity || (publish_blocked_ && pending_count_ != 0)) {
        return 0;
    }

    const std::uint64_t task_id = next_task_id_;
    TaskContextSlot &context = contexts_[task_id & capacity_mask_];
    if (context.state != TaskContextSlotState::free) {
        return EPROTO;
    }
    CopyTask task{};
    const int status = make_copy_task(config_, request, task_id, &task);
    if (status != 0) {
        return status;
    }

    context.task_id = task_id;
    context.parent_request_id = request.parent_request_id;
    context.payload_index = request.payload_index;
    context.state = TaskContextSlotState::pending;
    context.result = worker::DatagramResult::success;
    pending_tasks_[pending_count_] = task;
    if (pending_count_ == 0) {
        first_pending_nanoseconds_ = now_nanoseconds;
        pending_flushed_ = false;
    }
    ++pending_count_;
    ++outstanding_tasks_;
    ++next_task_id_;
    *accepted = true;

    if (pending_count_ == config_.host_batch) {
        return publish_pending();
    }
    return 0;
}

int PersistentCudaCopyHost::progress_host_batch(std::uint64_t now_nanoseconds) noexcept {
    if (!initialized()) {
        return EINVAL;
    }
    if (pending_count_ == 0) {
        return 0;
    }
    if (!publish_blocked_ && now_nanoseconds >= first_pending_nanoseconds_ &&
        now_nanoseconds - first_pending_nanoseconds_ < config_.max_batch_delay_nanoseconds) {
        return 0;
    }
    return publish_pending();
}

int PersistentCudaCopyHost::publish_pending_now() noexcept {
    if (!initialized()) {
        return EINVAL;
    }
    return pending_count_ == 0 ? 0 : publish_pending();
}

int PersistentCudaCopyHost::try_pop_completion(worker::BackendCompletion *completion,
                                               std::uint64_t now_nanoseconds,
                                               bool *completed) noexcept {
    if (completion == nullptr || completed == nullptr || !initialized()) {
        return EINVAL;
    }
    *completed = false;
    const int progress_status = progress_host_batch(now_nanoseconds);
    if (progress_status != 0) {
        return progress_status;
    }

    if (local_completion_head_ < local_completion_tail_) {
        const std::uint64_t task_id =
            local_completion_task_ids_[local_completion_head_ & capacity_mask_];
        TaskContextSlot &context = contexts_[task_id & capacity_mask_];
        if (context.state != TaskContextSlotState::local_error || context.task_id != task_id ||
            outstanding_tasks_ == 0) {
            last_publish_failure_ = HostPublishFailure::queue;
            return EPROTO;
        }
        completion->parent_request_id = context.parent_request_id;
        completion->payload_index = context.payload_index;
        completion->result = context.result;
        context = {};
        ++local_completion_head_;
        --outstanding_tasks_;
        *completed = true;
        return 0;
    }

    CopyCompletion copy_completion{};
    const int poll_status = queue_->try_poll(&copy_completion);
    if (poll_status == EAGAIN) {
        return 0;
    }
    if (poll_status != 0) {
        last_publish_failure_ = HostPublishFailure::queue;
        return poll_status;
    }
    TaskContextSlot &context = contexts_[copy_completion.task_id & capacity_mask_];
    if (context.state != TaskContextSlotState::submitted ||
        context.task_id != copy_completion.task_id || outstanding_tasks_ == 0) {
        last_publish_failure_ = HostPublishFailure::queue;
        return EPROTO;
    }

    completion->parent_request_id = context.parent_request_id;
    completion->payload_index = context.payload_index;
    completion->result = copy_completion.result == CopyTaskResult::success
                             ? worker::DatagramResult::success
                             : worker::DatagramResult::backend_error;
    context = {};
    --outstanding_tasks_;
    *completed = true;
    return 0;
}

int PersistentCudaCopyHost::try_complete_locally(const worker::BackendRequest &request,
                                                 worker::DatagramResult result,
                                                 bool *accepted) noexcept {
    if (accepted == nullptr || !initialized()) {
        return EINVAL;
    }
    *accepted = false;
    if (outstanding_tasks_ >= config_.queue_capacity) {
        return 0;
    }
    const std::uint64_t task_id = next_task_id_;
    TaskContextSlot &context = contexts_[task_id & capacity_mask_];
    if (context.state != TaskContextSlotState::free) {
        last_publish_failure_ = HostPublishFailure::queue;
        return EPROTO;
    }
    context.task_id = task_id;
    context.parent_request_id = request.parent_request_id;
    context.payload_index = request.payload_index;
    context.state = TaskContextSlotState::local_error;
    context.result = result;
    local_completion_task_ids_[local_completion_tail_ & capacity_mask_] = task_id;
    ++local_completion_tail_;
    ++outstanding_tasks_;
    ++next_task_id_;
    *accepted = true;
    return 0;
}

int PersistentCudaCopyHost::fail_pending(worker::DatagramResult result) noexcept {
    if (!initialized()) {
        return EINVAL;
    }
    for (std::size_t index = 0; index < pending_count_; ++index) {
        const std::uint64_t task_id = pending_tasks_[index].task_id;
        TaskContextSlot &context = contexts_[task_id & capacity_mask_];
        if (context.state != TaskContextSlotState::pending || context.task_id != task_id) {
            last_publish_failure_ = HostPublishFailure::queue;
            return EPROTO;
        }
        context.state = TaskContextSlotState::local_error;
        context.result = result;
        local_completion_task_ids_[local_completion_tail_ & capacity_mask_] = task_id;
        ++local_completion_tail_;
    }
    pending_count_ = 0;
    first_pending_nanoseconds_ = 0;
    publish_blocked_ = false;
    pending_flushed_ = false;
    last_publish_failure_ = HostPublishFailure::none;
    return 0;
}

int PersistentCudaCopyHost::fail_all(worker::DatagramResult result) noexcept {
    int status = fail_pending(result);
    if (status != 0) {
        return status;
    }
    for (std::size_t index = 0; index < config_.queue_capacity; ++index) {
        TaskContextSlot &context = contexts_[index];
        if (context.state != TaskContextSlotState::submitted) {
            continue;
        }
        context.state = TaskContextSlotState::local_error;
        context.result = result;
        local_completion_task_ids_[local_completion_tail_ & capacity_mask_] = context.task_id;
        ++local_completion_tail_;
    }
    last_publish_failure_ = HostPublishFailure::none;
    return 0;
}

int PersistentCudaCopyHost::reset() noexcept {
    if (!initialized()) {
        return 0;
    }
    if (outstanding_tasks_ != 0 || pending_count_ != 0 ||
        local_completion_head_ != local_completion_tail_) {
        return EBUSY;
    }
    config_ = {};
    queue_ = nullptr;
    visibility_gate_ = nullptr;
    contexts_.reset();
    local_completion_task_ids_.reset();
    pending_tasks_ = {};
    capacity_mask_ = 0;
    next_task_id_ = 1;
    local_completion_head_ = 0;
    local_completion_tail_ = 0;
    first_pending_nanoseconds_ = 0;
    publish_blocked_ = false;
    pending_flushed_ = false;
    last_publish_failure_ = HostPublishFailure::none;
    return 0;
}

std::size_t PersistentCudaCopyHost::outstanding_tasks() const noexcept {
    return outstanding_tasks_;
}

std::size_t PersistentCudaCopyHost::pending_tasks() const noexcept {
    return pending_count_;
}

std::size_t PersistentCudaCopyHost::local_completions() const noexcept {
    return static_cast<std::size_t>(local_completion_tail_ - local_completion_head_);
}

HostPublishFailure PersistentCudaCopyHost::last_publish_failure() const noexcept {
    return last_publish_failure_;
}

bool PersistentCudaCopyHost::initialized() const noexcept {
    return contexts_ != nullptr;
}

int PersistentCudaCopyHost::publish_pending() noexcept {
    last_publish_failure_ = HostPublishFailure::none;
    if (!pending_flushed_) {
        const int flush_status = visibility_gate_->flush_current_context_to_owner();
        if (flush_status != 0) {
            publish_blocked_ = true;
            last_publish_failure_ = HostPublishFailure::visibility;
            return flush_status;
        }
        pending_flushed_ = true;
    }

    std::size_t submitted_count = 0;
    const int status =
        queue_->try_submit_batch(pending_tasks_.data(), pending_count_, &submitted_count);
    if (status == EAGAIN) {
        publish_blocked_ = true;
        return 0;
    }
    if (status != 0) {
        last_publish_failure_ = HostPublishFailure::queue;
        return status;
    }
    if (submitted_count == 0 || submitted_count > pending_count_) {
        last_publish_failure_ = HostPublishFailure::queue;
        return EPROTO;
    }

    for (std::size_t index = 0; index < submitted_count; ++index) {
        TaskContextSlot &context = contexts_[pending_tasks_[index].task_id & capacity_mask_];
        if (context.state != TaskContextSlotState::pending ||
            context.task_id != pending_tasks_[index].task_id) {
            last_publish_failure_ = HostPublishFailure::queue;
            return EPROTO;
        }
        context.state = TaskContextSlotState::submitted;
    }

    pending_count_ -= submitted_count;
    if (pending_count_ != 0) {
        std::memmove(pending_tasks_.data(), pending_tasks_.data() + submitted_count,
                     pending_count_ * sizeof(CopyTask));
        publish_blocked_ = true;
    } else {
        first_pending_nanoseconds_ = 0;
        publish_blocked_ = false;
        pending_flushed_ = false;
    }
    return 0;
}

int CudaPersistentCopyRuntime::initialize_device(std::uint32_t device_ordinal) noexcept {
    if (device_initialized_) {
        return EINVAL;
    }
    const int status = initialize_persistent_copy_device(device_ordinal);
    if (status == 0) {
        device_initialized_ = true;
    }
    return status;
}

int CudaPersistentCopyRuntime::start(const PersistentCudaCopyBackendConfig &config) noexcept {
    if (!device_initialized_ || !queue_.empty()) {
        return EINVAL;
    }
    int status = WarpSpecializedQueue::allocate_pipeline(
        config.queue_capacity, kPersistentCudaCopyBackendCopyWarps,
        kPersistentCudaCopyBackendDeviceBatch, kPersistentCudaCopyBackendSharedQueueDepth,
        config.stage_buffer_base, &queue_);
    if (status != 0) {
        return status;
    }
    status = queue_.start_pipeline();
    if (status != 0) {
        (void)queue_.reset();
    }
    return status;
}

int CudaPersistentCopyRuntime::try_submit_batch(const CopyTask *tasks, std::size_t task_count,
                                                std::size_t *submitted_count) noexcept {
    return queue_.try_submit_batch(tasks, task_count, submitted_count);
}

int CudaPersistentCopyRuntime::try_poll(CopyCompletion *completion) noexcept {
    return queue_.try_poll(completion);
}

int CudaPersistentCopyRuntime::request_stop() noexcept {
    return queue_.request_stop();
}

int CudaPersistentCopyRuntime::wait() noexcept {
    return queue_.wait();
}

int CudaPersistentCopyRuntime::reset() noexcept {
    const int status = queue_.reset();
    device_initialized_ = false;
    return status;
}

std::uint64_t SteadyPersistentCopyClock::now_nanoseconds() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

PersistentCudaCopyBackend::PersistentCudaCopyBackend() noexcept
    : runtime_(&owned_runtime_), visibility_gate_(&owned_visibility_gate_), clock_(&owned_clock_) {
}

PersistentCudaCopyBackend::PersistentCudaCopyBackend(PersistentCopyRuntime *runtime,
                                                     GpuDirectVisibilityGate *visibility_gate,
                                                     PersistentCopyClock *clock) noexcept
    : runtime_(runtime), visibility_gate_(visibility_gate), clock_(clock) {
}

PersistentCudaCopyBackend::~PersistentCudaCopyBackend() {
    abort_and_reset();
}

int PersistentCudaCopyBackend::start(const PersistentCudaCopyBackendConfig &config) noexcept {
    if (state_ != PersistentCudaCopyBackendState::stopped || runtime_ == nullptr ||
        visibility_gate_ == nullptr || clock_ == nullptr ||
        validate_persistent_cuda_copy_backend_config(config) != 0) {
        return EINVAL;
    }
    int status = runtime_->initialize_device(config.device_ordinal);
    if (status != 0) {
        (void)runtime_->reset();
        last_error_ = status;
        return status;
    }
    status = visibility_gate_->initialize(config.device_ordinal);
    if (status != 0) {
        (void)visibility_gate_->reset();
        (void)runtime_->reset();
        last_error_ = status;
        return status;
    }
    status = host_.initialize(config, runtime_, visibility_gate_);
    if (status != 0) {
        (void)visibility_gate_->reset();
        (void)runtime_->reset();
        last_error_ = status;
        return status;
    }
    status = runtime_->start(config);
    if (status != 0) {
        (void)host_.reset();
        (void)visibility_gate_->reset();
        (void)runtime_->reset();
        last_error_ = status;
        return status;
    }

    config_ = config;
    state_ = PersistentCudaCopyBackendState::accepting;
    last_error_ = 0;
    runtime_stop_requested_ = false;
    return 0;
}

bool PersistentCudaCopyBackend::try_submit(const worker::BackendRequest &request) noexcept {
    if (state_ != PersistentCudaCopyBackendState::accepting) {
        return false;
    }
    bool accepted = false;
    const int status = host_.try_submit(request, clock_->now_nanoseconds(), &accepted);
    if (status == 0) {
        return accepted;
    }
    if (status == EINVAL && !accepted && host_.last_publish_failure() == HostPublishFailure::none) {
        const int local_status =
            host_.try_complete_locally(request, worker::DatagramResult::backend_error, &accepted);
        if (local_status != 0) {
            handle_host_error(local_status);
        }
        return accepted;
    }
    handle_host_error(status);
    return accepted;
}

bool PersistentCudaCopyBackend::try_pop_completion(worker::BackendCompletion &completion) noexcept {
    if (state_ == PersistentCudaCopyBackendState::stopped) {
        return false;
    }
    bool completed = false;
    int status = host_.try_pop_completion(&completion, clock_->now_nanoseconds(), &completed);
    if (status != 0) {
        handle_host_error(status);
        status = host_.try_pop_completion(&completion, clock_->now_nanoseconds(), &completed);
        if (status != 0) {
            last_error_ = status;
            completed = false;
        }
    }
    if (state_ == PersistentCudaCopyBackendState::draining) {
        const int stop_status = progress_stop_request();
        if (stop_status != 0) {
            last_error_ = stop_status;
        }
    }
    return completed;
}

int PersistentCudaCopyBackend::request_stop() noexcept {
    if (state_ != PersistentCudaCopyBackendState::accepting) {
        return EINVAL;
    }
    state_ = PersistentCudaCopyBackendState::draining;
    const int publish_status = host_.publish_pending_now();
    if (publish_status != 0) {
        handle_host_error(publish_status);
    }
    const int stop_status = progress_stop_request();
    if (stop_status != 0) {
        last_error_ = stop_status;
        return stop_status;
    }
    return publish_status;
}

int PersistentCudaCopyBackend::wait() noexcept {
    if (state_ != PersistentCudaCopyBackendState::draining) {
        return EINVAL;
    }
    const int stop_status = progress_stop_request();
    if (stop_status != 0) {
        last_error_ = stop_status;
        return stop_status;
    }
    if (!runtime_stop_requested_ || host_.outstanding_tasks() != 0 || host_.pending_tasks() != 0) {
        return EAGAIN;
    }
    int status = runtime_->wait();
    const int runtime_reset_status = runtime_->reset();
    const int host_reset_status = host_.reset();
    const int visibility_reset_status = visibility_gate_->reset();
    if (status == 0) {
        status = runtime_reset_status;
    }
    if (status == 0) {
        status = host_reset_status;
    }
    if (status == 0) {
        status = visibility_reset_status;
    }
    config_ = {};
    state_ = PersistentCudaCopyBackendState::stopped;
    runtime_stop_requested_ = false;
    if (status != 0) {
        last_error_ = status;
    }
    return status;
}

PersistentCudaCopyBackendState PersistentCudaCopyBackend::state() const noexcept {
    return state_;
}

std::size_t PersistentCudaCopyBackend::outstanding_tasks() const noexcept {
    return host_.outstanding_tasks();
}

int PersistentCudaCopyBackend::last_error() const noexcept {
    return last_error_;
}

void PersistentCudaCopyBackend::handle_host_error(int status) noexcept {
    last_error_ = status;
    const HostPublishFailure failure = host_.last_publish_failure();
    if (failure == HostPublishFailure::visibility) {
        const int failure_status = host_.fail_pending(worker::DatagramResult::backend_error);
        if (failure_status != 0) {
            last_error_ = failure_status;
            state_ = PersistentCudaCopyBackendState::draining;
        }
        return;
    }

    const int failure_status = host_.fail_all(worker::DatagramResult::backend_error);
    if (failure_status != 0) {
        last_error_ = failure_status;
    }
    state_ = PersistentCudaCopyBackendState::draining;
    const int stop_status = progress_stop_request();
    if (stop_status != 0) {
        last_error_ = stop_status;
    }
}

int PersistentCudaCopyBackend::progress_stop_request() noexcept {
    if (state_ != PersistentCudaCopyBackendState::draining || runtime_stop_requested_ ||
        host_.pending_tasks() != 0) {
        return 0;
    }
    const int status = runtime_->request_stop();
    if (status == 0) {
        runtime_stop_requested_ = true;
    }
    return status;
}

void PersistentCudaCopyBackend::abort_and_reset() noexcept {
    if (state_ == PersistentCudaCopyBackendState::stopped) {
        return;
    }
    (void)host_.fail_all(worker::DatagramResult::backend_error);
    if (!runtime_stop_requested_) {
        (void)runtime_->request_stop();
    }
    (void)runtime_->wait();
    (void)runtime_->reset();
    (void)visibility_gate_->reset();
    state_ = PersistentCudaCopyBackendState::stopped;
}

}  // namespace ugdr::gpu

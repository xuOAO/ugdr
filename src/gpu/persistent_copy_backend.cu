#include "gpu/persistent_copy_backend.hpp"

#include <cerrno>
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
                                       PersistentCopyQueue *queue) noexcept {
    if (initialized() || queue == nullptr ||
        validate_persistent_cuda_copy_backend_config(config) != 0 ||
        config.queue_capacity > std::numeric_limits<std::size_t>::max() / sizeof(TaskContextSlot)) {
        return EINVAL;
    }
    std::unique_ptr<TaskContextSlot[]> contexts(new (std::nothrow)
                                                    TaskContextSlot[config.queue_capacity]);
    if (contexts == nullptr) {
        return ENOMEM;
    }
    config_ = config;
    queue_ = queue;
    contexts_ = std::move(contexts);
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
    pending_tasks_[pending_count_] = task;
    if (pending_count_ == 0) {
        first_pending_nanoseconds_ = now_nanoseconds;
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

    CopyCompletion copy_completion{};
    const int poll_status = queue_->try_poll(&copy_completion);
    if (poll_status == EAGAIN) {
        return 0;
    }
    if (poll_status != 0) {
        return poll_status;
    }
    TaskContextSlot &context = contexts_[copy_completion.task_id & capacity_mask_];
    if (context.state != TaskContextSlotState::submitted ||
        context.task_id != copy_completion.task_id || outstanding_tasks_ == 0) {
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

std::size_t PersistentCudaCopyHost::outstanding_tasks() const noexcept {
    return outstanding_tasks_;
}

std::size_t PersistentCudaCopyHost::pending_tasks() const noexcept {
    return pending_count_;
}

bool PersistentCudaCopyHost::initialized() const noexcept {
    return contexts_ != nullptr;
}

int PersistentCudaCopyHost::publish_pending() noexcept {
    std::size_t submitted_count = 0;
    const int status =
        queue_->try_submit_batch(pending_tasks_.data(), pending_count_, &submitted_count);
    if (status == EAGAIN) {
        publish_blocked_ = true;
        return 0;
    }
    if (status != 0) {
        return status;
    }
    if (submitted_count == 0 || submitted_count > pending_count_) {
        return EPROTO;
    }

    for (std::size_t index = 0; index < submitted_count; ++index) {
        TaskContextSlot &context = contexts_[pending_tasks_[index].task_id & capacity_mask_];
        if (context.state != TaskContextSlotState::pending ||
            context.task_id != pending_tasks_[index].task_id) {
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
    }
    return 0;
}

}  // namespace ugdr::gpu

#include "gpu/cuda_batch_copy_backend.hpp"

#include <cuda_runtime_api.h>

#include <cerrno>
#include <cstdint>
#include <limits>

namespace ugdr::gpu {
namespace {

int cuda_status(cudaError_t status) noexcept {
    if (status == cudaSuccess) {
        return 0;
    }
    if (status == cudaErrorNotReady) {
        return EAGAIN;
    }
    if (status == cudaErrorInvalidValue || status == cudaErrorInvalidDevice) {
        return EINVAL;
    }
    if (status == cudaErrorMemoryAllocation) {
        return ENOMEM;
    }
    return EIO;
}

bool valid_request(const worker::BackendRequest &request) noexcept {
    return request.source_daemon_address != 0 && request.target_daemon_address != 0 &&
           request.payload_length != 0 &&
           request.source_daemon_address <=
               std::numeric_limits<std::uint64_t>::max() - request.payload_length &&
           request.target_daemon_address <=
               std::numeric_limits<std::uint64_t>::max() - request.payload_length;
}

}  // namespace

int validate_cuda_batch_copy_backend_config(const CudaBatchCopyBackendConfig &config) noexcept {
    if (config.max_batch != kCudaBatchCopyBackendMaxBatch ||
        config.slot_count != kCudaBatchCopyBackendSlotCount) {
        return EINVAL;
    }
    return 0;
}

int CudaBatchCopyRuntimeImpl::initialize_device(std::uint32_t device_ordinal) noexcept {
    if (device_ordinal > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return EINVAL;
    }
    return cuda_status(cudaSetDevice(static_cast<int>(device_ordinal)));
}

int CudaBatchCopyRuntimeImpl::create_stream(CudaBatchStreamHandle *stream) noexcept {
    if (stream == nullptr) {
        return EINVAL;
    }
    cudaStream_t cuda_stream = nullptr;
    const int status = cuda_status(cudaStreamCreateWithFlags(&cuda_stream, cudaStreamNonBlocking));
    *stream = status == 0 ? reinterpret_cast<CudaBatchStreamHandle>(cuda_stream) : nullptr;
    return status;
}

int CudaBatchCopyRuntimeImpl::destroy_stream(CudaBatchStreamHandle stream) noexcept {
    if (stream == nullptr) {
        return EINVAL;
    }
    return cuda_status(cudaStreamDestroy(reinterpret_cast<cudaStream_t>(stream)));
}

int CudaBatchCopyRuntimeImpl::create_event(CudaBatchEventHandle *event) noexcept {
    if (event == nullptr) {
        return EINVAL;
    }
    cudaEvent_t cuda_event = nullptr;
    const int status = cuda_status(cudaEventCreateWithFlags(&cuda_event, cudaEventDisableTiming));
    *event = status == 0 ? reinterpret_cast<CudaBatchEventHandle>(cuda_event) : nullptr;
    return status;
}

int CudaBatchCopyRuntimeImpl::destroy_event(CudaBatchEventHandle event) noexcept {
    if (event == nullptr) {
        return EINVAL;
    }
    return cuda_status(cudaEventDestroy(reinterpret_cast<cudaEvent_t>(event)));
}

int CudaBatchCopyRuntimeImpl::memcpy_batch_async(
    void **destinations, void **sources, std::size_t *sizes, std::size_t count,
    CudaBatchCopyAttributes *attributes, std::size_t *attribute_indices,
    std::size_t attribute_count, std::size_t *failed_index, CudaBatchStreamHandle stream) noexcept {
    if (destinations == nullptr || sources == nullptr || sizes == nullptr || count == 0 ||
        attributes == nullptr || attribute_indices == nullptr || attribute_count != 1 ||
        attribute_indices[0] != 0 || failed_index == nullptr || stream == nullptr ||
        attributes[0].source_access_order != CudaBatchSourceAccessOrder::stream ||
        attributes[0].flags != 0) {
        return EINVAL;
    }
    cudaMemcpyAttributes cuda_attributes{};
    cuda_attributes.srcAccessOrder = cudaMemcpySrcAccessOrderStream;
    cuda_attributes.flags = cudaMemcpyFlagDefault;
    std::size_t cuda_attribute_index = 0;
    return cuda_status(cudaMemcpyBatchAsync(destinations, sources, sizes, count, &cuda_attributes,
                                            &cuda_attribute_index, 1, failed_index,
                                            reinterpret_cast<cudaStream_t>(stream)));
}

int CudaBatchCopyRuntimeImpl::record_event(CudaBatchEventHandle event,
                                           CudaBatchStreamHandle stream) noexcept {
    if (event == nullptr || stream == nullptr) {
        return EINVAL;
    }
    return cuda_status(cudaEventRecord(reinterpret_cast<cudaEvent_t>(event),
                                       reinterpret_cast<cudaStream_t>(stream)));
}

int CudaBatchCopyRuntimeImpl::query_event(CudaBatchEventHandle event) noexcept {
    if (event == nullptr) {
        return EINVAL;
    }
    return cuda_status(cudaEventQuery(reinterpret_cast<cudaEvent_t>(event)));
}

int CudaBatchCopyRuntimeImpl::synchronize_stream(CudaBatchStreamHandle stream) noexcept {
    if (stream == nullptr) {
        return EINVAL;
    }
    return cuda_status(cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(stream)));
}

CudaBatchCopyBackend::CudaBatchCopyBackend() noexcept : runtime_(&owned_runtime_) {
}

CudaBatchCopyBackend::CudaBatchCopyBackend(CudaBatchCopyRuntime *runtime) noexcept
    : runtime_(runtime == nullptr ? &owned_runtime_ : runtime) {
}

CudaBatchCopyBackend::~CudaBatchCopyBackend() {
    if (state_ != CudaBatchCopyBackendState::stopped) {
        (void)runtime_->synchronize_stream(stream_);
        (void)destroy_resources();
    }
}

int CudaBatchCopyBackend::start(const CudaBatchCopyBackendConfig &config) noexcept {
    if (state_ != CudaBatchCopyBackendState::stopped ||
        validate_cuda_batch_copy_backend_config(config) != 0) {
        return EINVAL;
    }
    int status = runtime_->initialize_device(config.device_ordinal);
    if (status != 0) {
        last_error_ = status;
        return status;
    }
    status = runtime_->create_stream(&stream_);
    if (status != 0) {
        last_error_ = status;
        return status;
    }
    for (std::size_t index = 0; index < config.slot_count; ++index) {
        status = runtime_->create_event(&slots_[index].event);
        if (status != 0) {
            last_error_ = status;
            (void)destroy_resources();
            return status;
        }
    }
    config_ = config;
    state_ = CudaBatchCopyBackendState::accepting;
    last_error_ = 0;
    return 0;
}

bool CudaBatchCopyBackend::try_submit(const worker::BackendRequest &request) noexcept {
    if (state_ != CudaBatchCopyBackendState::accepting || pending_count_ == pending_.size() ||
        outstanding_count_ == completions_.size()) {
        return false;
    }
    const std::size_t tail = (pending_head_ + pending_count_) % pending_.size();
    pending_[tail] = request;
    ++pending_count_;
    ++outstanding_count_;
    return true;
}

std::size_t CudaBatchCopyBackend::try_submit_batch(const worker::BackendRequest *requests,
                                                   std::size_t request_count) noexcept {
    if (state_ != CudaBatchCopyBackendState::accepting ||
        (requests == nullptr && request_count != 0)) {
        return 0;
    }
    std::size_t accepted = 0;
    while (accepted != request_count && try_submit(requests[accepted])) {
        ++accepted;
    }
    return accepted;
}

bool CudaBatchCopyBackend::flush_submissions() noexcept {
    if (state_ == CudaBatchCopyBackendState::stopped) {
        return false;
    }
    const int status = progress();
    if (status != 0 && last_error_ == 0) {
        last_error_ = status;
    }
    return status == 0;
}

bool CudaBatchCopyBackend::try_pop_completion(worker::BackendCompletion &completion) noexcept {
    if (state_ == CudaBatchCopyBackendState::stopped) {
        return false;
    }
    const int status = progress();
    if (status != 0 && last_error_ == 0) {
        last_error_ = status;
    }
    if (completion_count_ == 0) {
        return false;
    }
    completion = completions_[completion_head_];
    completion_head_ = (completion_head_ + 1) % completions_.size();
    --completion_count_;
    --outstanding_count_;
    return true;
}

std::size_t
CudaBatchCopyBackend::try_pop_completion_batch(worker::BackendCompletion *completions,
                                               std::size_t completion_capacity) noexcept {
    if (state_ == CudaBatchCopyBackendState::stopped ||
        (completions == nullptr && completion_capacity != 0)) {
        return 0;
    }
    (void)progress();
    std::size_t count = 0;
    while (count != completion_capacity && completion_count_ != 0) {
        completions[count] = completions_[completion_head_];
        completion_head_ = (completion_head_ + 1) % completions_.size();
        --completion_count_;
        --outstanding_count_;
        ++count;
    }
    return count;
}

int CudaBatchCopyBackend::request_stop() noexcept {
    if (state_ != CudaBatchCopyBackendState::accepting) {
        return EINVAL;
    }
    state_ = CudaBatchCopyBackendState::draining;
    return progress();
}

int CudaBatchCopyBackend::wait() noexcept {
    if (state_ != CudaBatchCopyBackendState::draining) {
        return EINVAL;
    }
    const int progress_status = progress();
    if (pending_count_ != 0 || in_flight_count_ != 0 || completion_count_ != 0 ||
        outstanding_count_ != 0) {
        return EAGAIN;
    }
    const int destroy_status = destroy_resources();
    state_ = CudaBatchCopyBackendState::stopped;
    config_ = {};
    if (progress_status != 0) {
        return progress_status;
    }
    return destroy_status;
}

CudaBatchCopyBackendState CudaBatchCopyBackend::state() const noexcept {
    return state_;
}

std::size_t CudaBatchCopyBackend::pending_tasks() const noexcept {
    return pending_count_;
}

std::size_t CudaBatchCopyBackend::in_flight_batches() const noexcept {
    return in_flight_count_;
}

std::size_t CudaBatchCopyBackend::outstanding_tasks() const noexcept {
    return outstanding_count_;
}

std::uint64_t CudaBatchCopyBackend::submitted_batches() const noexcept {
    return submitted_batches_;
}

std::uint64_t CudaBatchCopyBackend::submitted_payloads() const noexcept {
    return submitted_payloads_;
}

std::uint64_t CudaBatchCopyBackend::event_queries() const noexcept {
    return event_queries_;
}

std::uint64_t CudaBatchCopyBackend::event_not_ready_queries() const noexcept {
    return event_not_ready_queries_;
}

int CudaBatchCopyBackend::last_error() const noexcept {
    return last_error_;
}

int CudaBatchCopyBackend::progress() noexcept {
    if (state_ == CudaBatchCopyBackendState::stopped) {
        return EINVAL;
    }

    int status = 0;
    if (pending_count_ != 0 && find_free_slot() == nullptr) {
        status = poll_completed_prefix_newest_to_oldest();
        if (status != 0 && last_error_ == 0) {
            last_error_ = status;
        }
        const int publish_status = publish_terminal_prefix();
        if (status == 0) {
            status = publish_status;
        }
    }

    while (pending_count_ != 0) {
        CudaBatchSlot *const slot = find_free_slot();
        if (slot == nullptr) {
            break;
        }
        const int submit_status =
            last_error_ == 0 ? submit_one_batch(*slot) : fail_one_pending_batch(*slot);
        if (submit_status != 0 && status == 0) {
            status = submit_status;
        }
        const int poll_status = poll_completed_prefix_newest_to_oldest();
        if (poll_status != 0 && status == 0) {
            status = poll_status;
        }
        const int publish_status = publish_terminal_prefix();
        if (publish_status != 0 && status == 0) {
            status = publish_status;
        }
    }

    const int poll_status = poll_completed_prefix_newest_to_oldest();
    if (poll_status != 0 && status == 0) {
        status = poll_status;
    }
    const int publish_status = publish_terminal_prefix();
    if (publish_status != 0 && status == 0) {
        status = publish_status;
    }
    return status;
}

int CudaBatchCopyBackend::submit_one_batch(CudaBatchSlot &slot) noexcept {
    slot.state = CudaBatchSlotState::preparing;
    const std::uint64_t parent_id = pending_at(0).parent_request_id;
    std::size_t count = 0;
    bool valid = true;
    while (count < config_.max_batch && count < pending_count_ &&
           pending_at(count).parent_request_id == parent_id) {
        const worker::BackendRequest &request = pending_at(count);
        valid = valid && valid_request(request);
        slot.destinations[count] =
            reinterpret_cast<void *>(static_cast<std::uintptr_t>(request.target_daemon_address));
        slot.sources[count] =
            reinterpret_cast<void *>(static_cast<std::uintptr_t>(request.source_daemon_address));
        slot.sizes[count] = request.payload_length;
        ++count;
    }

    if (!valid) {
        for (std::size_t index = 0; index < count; ++index) {
            slot.requests[index] = pending_at(index);
        }
        pop_pending(count);
        slot.count = count;
        slot.sequence = next_sequence_++;
        slot.state = CudaBatchSlotState::failed;
        ++in_flight_count_;
        return 0;
    }

    slot.attributes = {};
    slot.attributes.source_access_order = CudaBatchSourceAccessOrder::stream;
    slot.attributes.flags = 0;
    slot.attribute_index = 0;
    std::size_t failed_index = std::numeric_limits<std::size_t>::max();
    int status = runtime_->memcpy_batch_async(slot.destinations.data(), slot.sources.data(),
                                              slot.sizes.data(), count, &slot.attributes,
                                              &slot.attribute_index, 1, &failed_index, stream_);
    if (status == 0) {
        ++submitted_batches_;
        submitted_payloads_ += count;
        status = runtime_->record_event(slot.event, stream_);
    }

    for (std::size_t index = 0; index < count; ++index) {
        slot.requests[index] = pending_at(index);
    }
    pop_pending(count);
    slot.count = count;
    slot.sequence = next_sequence_++;
    slot.state = status == 0 ? CudaBatchSlotState::enqueued : CudaBatchSlotState::failed;
    ++in_flight_count_;
    if (status != 0) {
        return enter_runtime_error(status);
    }
    return 0;
}

int CudaBatchCopyBackend::fail_one_pending_batch(CudaBatchSlot &slot) noexcept {
    slot.state = CudaBatchSlotState::preparing;
    const std::uint64_t parent_id = pending_at(0).parent_request_id;
    std::size_t count = 0;
    while (count < config_.max_batch && count < pending_count_ &&
           pending_at(count).parent_request_id == parent_id) {
        slot.requests[count] = pending_at(count);
        ++count;
    }
    pop_pending(count);
    slot.count = count;
    slot.sequence = next_sequence_++;
    slot.state = CudaBatchSlotState::failed;
    ++in_flight_count_;
    return 0;
}

int CudaBatchCopyBackend::poll_completed_prefix_newest_to_oldest() noexcept {
    if (in_flight_count_ == 0 || last_error_ != 0) {
        return 0;
    }
    std::uint64_t sequence = next_sequence_ - 1;
    while (sequence >= oldest_sequence_) {
        CudaBatchSlot *const slot = find_slot_by_sequence(sequence);
        if (slot == nullptr) {
            return EPROTO;
        }
        if (slot->state == CudaBatchSlotState::completed) {
            for (CudaBatchSlot &candidate : slots_) {
                if (candidate.state == CudaBatchSlotState::enqueued &&
                    candidate.sequence <= sequence) {
                    candidate.state = CudaBatchSlotState::completed;
                }
            }
            return 0;
        }
        if (slot->state == CudaBatchSlotState::failed) {
            if (sequence == oldest_sequence_) {
                break;
            }
            --sequence;
            continue;
        }
        if (slot->state != CudaBatchSlotState::enqueued) {
            return EPROTO;
        }
        ++event_queries_;
        const int status = runtime_->query_event(slot->event);
        if (status == 0) {
            for (CudaBatchSlot &candidate : slots_) {
                if (candidate.state == CudaBatchSlotState::enqueued &&
                    candidate.sequence <= sequence) {
                    candidate.state = CudaBatchSlotState::completed;
                }
            }
            return 0;
        }
        if (status != EAGAIN) {
            return enter_runtime_error(status);
        }
        ++event_not_ready_queries_;
        if (sequence == oldest_sequence_) {
            break;
        }
        --sequence;
    }
    return 0;
}

int CudaBatchCopyBackend::publish_terminal_prefix() noexcept {
    while (in_flight_count_ != 0) {
        CudaBatchSlot *const slot = find_slot_by_sequence(oldest_sequence_);
        if (slot == nullptr) {
            return EPROTO;
        }
        if (slot->state != CudaBatchSlotState::completed &&
            slot->state != CudaBatchSlotState::failed) {
            break;
        }
        const worker::DatagramResult result = slot->state == CudaBatchSlotState::completed
                                                  ? worker::DatagramResult::success
                                                  : worker::DatagramResult::backend_error;
        for (std::size_t index = 0; index < slot->count; ++index) {
            if (!enqueue_completion(slot->requests[index], result)) {
                return EOVERFLOW;
            }
        }
        const CudaBatchEventHandle event = slot->event;
        *slot = {};
        slot->event = event;
        --in_flight_count_;
        ++oldest_sequence_;
    }
    return 0;
}

int CudaBatchCopyBackend::enter_runtime_error(int status) noexcept {
    if (status == 0) {
        status = EIO;
    }
    if (last_error_ == 0) {
        last_error_ = status;
    }
    state_ = CudaBatchCopyBackendState::draining;
    const int sync_status = synchronize_failed_stream();
    for (CudaBatchSlot &slot : slots_) {
        if (slot.state == CudaBatchSlotState::enqueued) {
            slot.state = CudaBatchSlotState::failed;
        }
    }
    return sync_status == 0 ? status : sync_status;
}

int CudaBatchCopyBackend::synchronize_failed_stream() noexcept {
    if (stream_ == nullptr) {
        return 0;
    }
    const int status = runtime_->synchronize_stream(stream_);
    if (status != 0 && last_error_ == 0) {
        last_error_ = status;
    }
    return status;
}

int CudaBatchCopyBackend::destroy_resources() noexcept {
    int status = 0;
    for (CudaBatchSlot &slot : slots_) {
        if (slot.event != nullptr) {
            const int event_status = runtime_->destroy_event(slot.event);
            if (status == 0) {
                status = event_status;
            }
        }
        slot = {};
    }
    if (stream_ != nullptr) {
        const int stream_status = runtime_->destroy_stream(stream_);
        if (status == 0) {
            status = stream_status;
        }
    }
    stream_ = nullptr;
    pending_head_ = 0;
    pending_count_ = 0;
    completion_head_ = 0;
    completion_count_ = 0;
    in_flight_count_ = 0;
    outstanding_count_ = 0;
    oldest_sequence_ = 1;
    next_sequence_ = 1;
    return status;
}

CudaBatchSlot *CudaBatchCopyBackend::find_free_slot() noexcept {
    for (CudaBatchSlot &slot : slots_) {
        if (slot.state == CudaBatchSlotState::free) {
            return &slot;
        }
    }
    return nullptr;
}

CudaBatchSlot *CudaBatchCopyBackend::find_slot_by_sequence(std::uint64_t sequence) noexcept {
    for (CudaBatchSlot &slot : slots_) {
        if (slot.state != CudaBatchSlotState::free && slot.sequence == sequence) {
            return &slot;
        }
    }
    return nullptr;
}

const worker::BackendRequest &CudaBatchCopyBackend::pending_at(std::size_t offset) const noexcept {
    return pending_[(pending_head_ + offset) % pending_.size()];
}

void CudaBatchCopyBackend::pop_pending(std::size_t count) noexcept {
    pending_head_ = (pending_head_ + count) % pending_.size();
    pending_count_ -= count;
}

bool CudaBatchCopyBackend::enqueue_completion(const worker::BackendRequest &request,
                                              worker::DatagramResult result) noexcept {
    if (completion_count_ == completions_.size()) {
        return false;
    }
    const std::size_t tail = (completion_head_ + completion_count_) % completions_.size();
    completions_[tail] = {request.parent_request_id, request.payload_index, result};
    ++completion_count_;
    return true;
}

}  // namespace ugdr::gpu

#include "support/loop_worker_fixture.hpp"

#include <cstddef>
#include <iterator>
#include <utility>

namespace ugdr::test {

ScriptedCopyBackend::ScriptedCopyBackend(std::size_t capacity) : capacity_(capacity) {
}

bool ScriptedCopyBackend::try_submit(const worker::BackendRequest &request) {
    if (accepted_.size() >= capacity_) {
        return false;
    }
    accepted_.push_back(request);
    return true;
}

std::size_t ScriptedCopyBackend::try_submit_batch(const worker::BackendRequest *requests,
                                                  std::size_t request_count) {
    ++submit_batch_count_;
    return CopyBackend::try_submit_batch(requests, request_count);
}

bool ScriptedCopyBackend::flush_submissions() {
    ++flush_count_;
    return true;
}

bool ScriptedCopyBackend::try_pop_completion(worker::BackendCompletion &completion) {
    if (completions_.empty()) {
        return false;
    }
    completion = completions_.front();
    completions_.pop_front();
    return true;
}

std::size_t ScriptedCopyBackend::try_pop_completion_batch(worker::BackendCompletion *completions,
                                                          std::size_t completion_capacity) {
    ++completion_batch_count_;
    return CopyBackend::try_pop_completion_batch(completions, completion_capacity);
}

bool ScriptedCopyBackend::progress_once(worker::DatagramResult result,
                                        worker::BackendRequest *completed_request) {
    return progress_at(0, result, completed_request);
}

bool ScriptedCopyBackend::progress_at(std::size_t index, worker::DatagramResult result,
                                      worker::BackendRequest *completed_request) {
    if (index >= accepted_.size() || completions_.size() >= capacity_) {
        return false;
    }
    auto request = accepted_.begin();
    std::advance(request, static_cast<std::ptrdiff_t>(index));
    if (completed_request != nullptr) {
        *completed_request = *request;
    }
    completions_.push_back({request->parent_request_id, request->payload_index, result});
    accepted_.erase(request);
    return true;
}

bool ScriptedCopyBackend::inject_completion(const worker::BackendCompletion &completion) {
    if (completions_.size() >= capacity_) {
        return false;
    }
    completions_.push_back(completion);
    return true;
}

void ScriptedCopyBackend::set_capacity(std::size_t capacity) noexcept {
    capacity_ = capacity;
}

std::size_t ScriptedCopyBackend::accepted_count() const noexcept {
    return accepted_.size();
}

std::size_t ScriptedCopyBackend::submit_batch_count() const noexcept {
    return submit_batch_count_;
}

std::size_t ScriptedCopyBackend::completion_batch_count() const noexcept {
    return completion_batch_count_;
}

std::size_t ScriptedCopyBackend::flush_count() const noexcept {
    return flush_count_;
}

const worker::BackendRequest *ScriptedCopyBackend::front_request() const noexcept {
    return accepted_.empty() ? nullptr : &accepted_.front();
}

}  // namespace ugdr::test

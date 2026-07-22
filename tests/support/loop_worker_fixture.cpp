#include "support/loop_worker_fixture.hpp"

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

bool ScriptedCopyBackend::try_pop_completion(worker::BackendCompletion &completion) {
    if (completions_.empty()) {
        return false;
    }
    completion = completions_.front();
    completions_.pop_front();
    return true;
}

bool ScriptedCopyBackend::progress_once(worker::DatagramResult result) {
    if (accepted_.empty() || completions_.size() >= capacity_) {
        return false;
    }
    completions_.push_back({accepted_.front().request_id, result});
    accepted_.pop_front();
    return true;
}

void ScriptedCopyBackend::set_capacity(std::size_t capacity) noexcept {
    capacity_ = capacity;
}

std::size_t ScriptedCopyBackend::accepted_count() const noexcept {
    return accepted_.size();
}

const worker::BackendRequest *ScriptedCopyBackend::front_request() const noexcept {
    return accepted_.empty() ? nullptr : &accepted_.front();
}

}  // namespace ugdr::test

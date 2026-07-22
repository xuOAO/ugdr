#include "support/loop_worker_fixture.hpp"

#include <cuda_runtime_api.h>

#include <cstdint>

namespace ugdr::test {

MockGpuBackend::MockGpuBackend(std::size_t capacity) : capacity_(capacity) {
}

bool MockGpuBackend::try_submit(const worker::BackendRequest &request) {
    if (accepted_.size() >= capacity_) {
        return false;
    }
    accepted_.push_back(request);
    return true;
}

bool MockGpuBackend::try_pop_completion(worker::BackendCompletion &completion) {
    if (completions_.empty()) {
        return false;
    }
    completion = completions_.front();
    completions_.pop_front();
    return true;
}

bool MockGpuBackend::progress_once(worker::DatagramResult result) {
    if (accepted_.empty() || completions_.size() >= capacity_) {
        return false;
    }
    const worker::BackendRequest &request = accepted_.front();
    worker::DatagramResult completion_result = result;
    if (result == worker::DatagramResult::success) {
        std::uint64_t destination = request.target_daemon_address;
        for (const worker::ResolvedSourceSegment &source : request.source_segments) {
            const auto copy_status = cudaMemcpy(
                reinterpret_cast<void *>(static_cast<std::uintptr_t>(destination)),
                reinterpret_cast<const void *>(static_cast<std::uintptr_t>(source.daemon_address)),
                source.length, cudaMemcpyDeviceToDevice);
            if (copy_status != cudaSuccess) {
                completion_result = worker::DatagramResult::backend_error;
                break;
            }
            destination += source.length;
        }
    }
    completions_.push_back({request.request_id, completion_result});
    accepted_.pop_front();
    return true;
}

std::size_t MockGpuBackend::accepted_count() const noexcept {
    return accepted_.size();
}

}  // namespace ugdr::test

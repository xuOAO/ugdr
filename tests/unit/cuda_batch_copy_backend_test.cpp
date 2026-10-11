#include "gpu/cuda_batch_copy_backend.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using ugdr::gpu::CudaBatchCopyAttributes;
using ugdr::gpu::CudaBatchCopyBackend;
using ugdr::gpu::CudaBatchCopyBackendConfig;
using ugdr::gpu::CudaBatchCopyBackendState;
using ugdr::gpu::CudaBatchCopyRuntime;
using ugdr::gpu::CudaBatchEventHandle;
using ugdr::gpu::CudaBatchSourceAccessOrder;
using ugdr::gpu::CudaBatchStreamHandle;
using ugdr::worker::BackendCompletion;
using ugdr::worker::BackendRequest;
using ugdr::worker::DatagramResult;

class FakeRuntime final : public CudaBatchCopyRuntime {
  public:
    int initialize_device(std::uint32_t device_ordinal) noexcept override {
        calls.push_back("initialize");
        initialized_device = device_ordinal;
        return initialize_status;
    }

    int create_stream(CudaBatchStreamHandle *stream) noexcept override {
        calls.push_back("create_stream");
        if (create_stream_status != 0) {
            return create_stream_status;
        }
        *stream = reinterpret_cast<void *>(static_cast<std::uintptr_t>(1));
        return 0;
    }

    int destroy_stream(CudaBatchStreamHandle) noexcept override {
        calls.push_back("destroy_stream");
        ++destroy_stream_calls;
        return destroy_stream_status;
    }

    int create_event(CudaBatchEventHandle *event) noexcept override {
        calls.push_back("create_event");
        if (create_event_calls == fail_event_create_index) {
            ++create_event_calls;
            return create_event_status;
        }
        *event = reinterpret_cast<void *>(static_cast<std::uintptr_t>(101) + create_event_calls);
        ++create_event_calls;
        return 0;
    }

    int destroy_event(CudaBatchEventHandle) noexcept override {
        calls.push_back("destroy_event");
        ++destroy_event_calls;
        return destroy_event_status;
    }

    int memcpy_batch_async(void **destinations, void **sources, std::size_t *sizes,
                           std::size_t count, CudaBatchCopyAttributes *attributes,
                           std::size_t *attribute_indices, std::size_t attribute_count,
                           std::size_t *failed_index, CudaBatchStreamHandle) noexcept override {
        calls.push_back("batch");
        if (destinations == nullptr || sources == nullptr || sizes == nullptr || count == 0 ||
            attributes == nullptr || attribute_indices == nullptr || attribute_count != 1 ||
            attribute_indices[0] != 0 || failed_index == nullptr ||
            attributes[0].source_access_order != CudaBatchSourceAccessOrder::stream ||
            attributes[0].flags != 0) {
            return EINVAL;
        }
        *failed_index = std::numeric_limits<std::size_t>::max();
        batches.push_back(count);
        first_sources.push_back(reinterpret_cast<std::uintptr_t>(sources[0]));
        first_destinations.push_back(reinterpret_cast<std::uintptr_t>(destinations[0]));
        return batch_status;
    }

    int record_event(CudaBatchEventHandle event, CudaBatchStreamHandle) noexcept override {
        calls.push_back("record");
        recorded_events.push_back(event_id(event));
        return record_status;
    }

    int query_event(CudaBatchEventHandle event) noexcept override {
        const std::size_t id = event_id(event);
        calls.push_back("query" + std::to_string(id));
        queried_events.push_back(id);
        return query_statuses[id];
    }

    int synchronize_stream(CudaBatchStreamHandle) noexcept override {
        calls.push_back("synchronize");
        ++synchronize_calls;
        return synchronize_status;
    }

    static std::size_t event_id(CudaBatchEventHandle event) noexcept {
        return reinterpret_cast<std::uintptr_t>(event) - static_cast<std::uintptr_t>(101);
    }

    std::uint32_t initialized_device = 0;
    int initialize_status = 0;
    int create_stream_status = 0;
    int create_event_status = EIO;
    std::size_t fail_event_create_index = std::numeric_limits<std::size_t>::max();
    int batch_status = 0;
    int record_status = 0;
    int synchronize_status = 0;
    int destroy_event_status = 0;
    int destroy_stream_status = 0;
    std::size_t create_event_calls = 0;
    std::size_t destroy_event_calls = 0;
    std::size_t destroy_stream_calls = 0;
    std::size_t synchronize_calls = 0;
    std::array<int, ugdr::gpu::kCudaBatchCopyBackendSlotCount> query_statuses{EAGAIN, EAGAIN,
                                                                              EAGAIN, EAGAIN};
    std::vector<std::string> calls;
    std::vector<std::size_t> batches;
    std::vector<std::size_t> first_sources;
    std::vector<std::size_t> first_destinations;
    std::vector<std::size_t> recorded_events;
    std::vector<std::size_t> queried_events;
};

BackendRequest request(std::uint64_t parent, std::uint32_t payload_index,
                       std::uint64_t address_index = 0) {
    BackendRequest value;
    value.parent_request_id = parent;
    value.parent_total_length = 128 * 1024;
    value.payload_offset = static_cast<std::uint64_t>(payload_index) * 1024;
    value.source_daemon_address = UINT64_C(0x100000) + address_index * 4096;
    value.target_daemon_address = UINT64_C(0x200000) + address_index * 4096;
    value.payload_length = 1024;
    value.payload_index = payload_index;
    value.payload_count = 65;
    return value;
}

int config_and_start_rollback_test() {
    CudaBatchCopyBackendConfig config;
    if (ugdr::gpu::validate_cuda_batch_copy_backend_config(config) != 0) {
        return 1;
    }
    config.max_batch = 63;
    if (ugdr::gpu::validate_cuda_batch_copy_backend_config(config) != EINVAL) {
        return 2;
    }
    config = {};
    config.slot_count = 3;
    if (ugdr::gpu::validate_cuda_batch_copy_backend_config(config) != EINVAL) {
        return 3;
    }

    FakeRuntime runtime;
    runtime.fail_event_create_index = 2;
    CudaBatchCopyBackend backend(&runtime);
    if (backend.start() != EIO || backend.state() != CudaBatchCopyBackendState::stopped ||
        runtime.destroy_event_calls != 2 || runtime.destroy_stream_calls != 1) {
        return 4;
    }
    return 0;
}

int fixed_batch_and_fifo_completion_test() {
    FakeRuntime runtime;
    CudaBatchCopyBackend backend(&runtime);
    if (backend.start() != 0) {
        return 1;
    }
    std::array<BackendRequest, 67> requests{};
    for (std::size_t index = 0; index < 65; ++index) {
        requests[index] = request(10, static_cast<std::uint32_t>(index), index);
    }
    requests[65] = request(11, 0, 65);
    requests[66] = request(11, 1, 66);
    if (backend.try_submit_batch(requests.data(), requests.size()) != requests.size() ||
        !backend.flush_submissions() || runtime.batches != std::vector<std::size_t>({64, 1, 2}) ||
        runtime.recorded_events != std::vector<std::size_t>({0, 1, 2}) ||
        backend.in_flight_batches() != 3 || backend.submitted_batches() != 3) {
        return 2;
    }

    runtime.calls.clear();
    runtime.queried_events.clear();
    runtime.query_statuses[2] = 0;
    std::array<BackendCompletion, 67> completions{};
    if (backend.try_pop_completion_batch(completions.data(), completions.size()) !=
            completions.size() ||
        runtime.queried_events.empty() || runtime.queried_events.front() != 2 ||
        backend.in_flight_batches() != 0) {
        return 3;
    }
    for (std::size_t index = 0; index < 65; ++index) {
        if (completions[index].parent_request_id != 10 ||
            completions[index].payload_index != index ||
            completions[index].result != DatagramResult::success) {
            return 4;
        }
    }
    if (completions[65].parent_request_id != 11 || completions[65].payload_index != 0 ||
        completions[66].parent_request_id != 11 || completions[66].payload_index != 1) {
        return 5;
    }
    if (backend.request_stop() != 0 || backend.wait() != 0 ||
        backend.state() != CudaBatchCopyBackendState::stopped) {
        return 6;
    }
    return 0;
}

int fixed_batch_size_matrix_test() {
    for (const std::size_t payload_count :
         {std::size_t{1}, std::size_t{63}, std::size_t{64}, std::size_t{65}}) {
        FakeRuntime runtime;
        CudaBatchCopyBackend backend(&runtime);
        if (backend.start() != 0) {
            return 1;
        }
        std::vector<BackendRequest> requests(payload_count);
        for (std::size_t index = 0; index < requests.size(); ++index) {
            requests[index] = request(50, static_cast<std::uint32_t>(index), index);
        }
        if (backend.try_submit_batch(requests.data(), requests.size()) != requests.size() ||
            !backend.flush_submissions()) {
            return 2;
        }
        const std::vector<std::size_t> expected = payload_count == 65
                                                      ? std::vector<std::size_t>{64, 1}
                                                      : std::vector<std::size_t>{payload_count};
        if (runtime.batches != expected || backend.submitted_payloads() != payload_count) {
            return 3;
        }
        runtime.query_statuses = {0, 0, 0, 0};
        std::vector<BackendCompletion> completions(payload_count);
        if (backend.try_pop_completion_batch(completions.data(), completions.size()) !=
            completions.size()) {
            return 4;
        }
        if (backend.request_stop() != 0 || backend.wait() != 0) {
            return 5;
        }
    }
    return 0;
}

int full_slot_prequery_test() {
    FakeRuntime runtime;
    CudaBatchCopyBackend backend(&runtime);
    if (backend.start() != 0) {
        return 1;
    }
    for (std::size_t index = 0; index < 4; ++index) {
        const BackendRequest item = request(100 + index, 0, index);
        if (!backend.try_submit(item) || !backend.flush_submissions()) {
            return 2;
        }
    }
    const BackendRequest fifth = request(200, 0, 4);
    if (!backend.try_submit(fifth)) {
        return 3;
    }
    runtime.calls.clear();
    runtime.queried_events.clear();
    runtime.query_statuses[1] = 0;
    if (!backend.flush_submissions() || runtime.queried_events.size() < 3 ||
        runtime.queried_events[0] != 3 || runtime.queried_events[1] != 2 ||
        runtime.queried_events[2] != 1 || runtime.batches.size() != 5 || runtime.calls.size() < 5 ||
        runtime.calls[0] != "query3" || runtime.calls[1] != "query2" ||
        runtime.calls[2] != "query1" || runtime.calls[3] != "batch" ||
        runtime.calls[4] != "record") {
        return 4;
    }
    runtime.query_statuses = {0, 0, 0, 0};
    std::array<BackendCompletion, 5> completions{};
    if (backend.try_pop_completion_batch(completions.data(), completions.size()) != 5) {
        return 5;
    }
    for (std::size_t index = 0; index < 4; ++index) {
        if (completions[index].parent_request_id != 100 + index) {
            return 6;
        }
    }
    if (completions[4].parent_request_id != 200) {
        return 7;
    }
    if (backend.request_stop() != 0 || backend.wait() != 0) {
        return 8;
    }
    return 0;
}

int stop_wait_is_nonblocking_test() {
    FakeRuntime runtime;
    CudaBatchCopyBackend backend(&runtime);
    if (backend.start() != 0) {
        return 1;
    }
    const BackendRequest item = request(250, 0, 0);
    if (!backend.try_submit(item) || !backend.flush_submissions() || backend.request_stop() != 0 ||
        backend.wait() != EAGAIN) {
        return 2;
    }
    runtime.query_statuses[0] = 0;
    BackendCompletion completion;
    if (!backend.try_pop_completion(completion) || completion.result != DatagramResult::success ||
        backend.wait() != 0 || backend.state() != CudaBatchCopyBackendState::stopped) {
        return 3;
    }
    return 0;
}

int runtime_error_and_drain_test() {
    FakeRuntime batch_runtime;
    CudaBatchCopyBackend batch_backend(&batch_runtime);
    if (batch_backend.start() != 0) {
        return 1;
    }
    batch_runtime.batch_status = EIO;
    const BackendRequest item = request(300, 0, 0);
    if (!batch_backend.try_submit(item) || batch_backend.flush_submissions() ||
        batch_backend.state() != CudaBatchCopyBackendState::draining ||
        batch_backend.last_error() != EIO || !batch_runtime.recorded_events.empty() ||
        batch_runtime.synchronize_calls != 1) {
        return 2;
    }
    BackendCompletion completion;
    if (!batch_backend.try_pop_completion(completion) ||
        completion.result != DatagramResult::backend_error || batch_backend.wait() != 0) {
        return 3;
    }

    FakeRuntime runtime;
    CudaBatchCopyBackend backend(&runtime);
    if (backend.start() != 0) {
        return 4;
    }
    runtime.record_status = EIO;
    if (!backend.try_submit(item) || backend.flush_submissions() ||
        backend.state() != CudaBatchCopyBackendState::draining || backend.last_error() != EIO ||
        runtime.synchronize_calls != 1) {
        return 5;
    }
    if (!backend.try_pop_completion(completion) || completion.parent_request_id != 300 ||
        completion.result != DatagramResult::backend_error || backend.wait() != 0) {
        return 6;
    }

    FakeRuntime query_runtime;
    CudaBatchCopyBackend query_backend(&query_runtime);
    if (query_backend.start() != 0) {
        return 7;
    }
    query_runtime.query_statuses[0] = EIO;
    if (!query_backend.try_submit(item) || query_backend.flush_submissions() ||
        query_backend.state() != CudaBatchCopyBackendState::draining ||
        !query_backend.try_pop_completion(completion) ||
        completion.result != DatagramResult::backend_error || query_backend.wait() != 0) {
        return 8;
    }
    return 0;
}

}  // namespace

int main() {
    int status = config_and_start_rollback_test();
    if (status != 0) {
        return 10 + status;
    }
    status = fixed_batch_and_fifo_completion_test();
    if (status != 0) {
        return 20 + status;
    }
    status = fixed_batch_size_matrix_test();
    if (status != 0) {
        return 30 + status;
    }
    status = full_slot_prequery_test();
    if (status != 0) {
        return 40 + status;
    }
    status = stop_wait_is_nonblocking_test();
    if (status != 0) {
        return 50 + status;
    }
    status = runtime_error_and_drain_test();
    return status == 0 ? 0 : 60 + status;
}

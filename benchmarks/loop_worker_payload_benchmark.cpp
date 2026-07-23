#include "api/wr_posting.hpp"
#include "control/qp.hpp"
#include "queue/descriptors.hpp"
#include "worker/worker.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

ugdr::control::DecodedControlRequest decoded(ugdr::control::UgdrControlRequest request) {
    ugdr::control::DecodedControlRequest value;
    value.value = std::move(request);
    return value;
}

class FakeCudaMemoryBackend final : public ugdr::gpu::CudaIpcMemoryBackend {
  public:
    int open(const ugdr::gpu::ExportedCudaMemory &memory,
             ugdr::gpu::CudaIpcMapping *mapping) override {
        mapping->gpu_uuid = memory.gpu_uuid;
        mapping->daemon_base_address = next_address_;
        next_address_ += UINT64_C(0x10000000);
        return 0;
    }

    int close(const ugdr::gpu::CudaIpcMapping &) noexcept override {
        return 0;
    }

  private:
    std::uint64_t next_address_ = UINT64_C(0x800000000);
};

class ControlOnlyBackend final : public ugdr::worker::CopyBackend {
  public:
    explicit ControlOnlyBackend(std::size_t capacity) : capacity_(capacity) {
    }

    bool try_submit(const ugdr::worker::BackendRequest &request) override {
        if (accepted_.size() >= capacity_) {
            return false;
        }
        accepted_.push_back(request);
        return true;
    }

    bool try_pop_completion(ugdr::worker::BackendCompletion &completion) override {
        if (completions_.empty()) {
            return false;
        }
        completion = completions_.front();
        completions_.pop_front();
        return true;
    }

    bool progress_once() {
        if (accepted_.empty() || completions_.size() >= capacity_) {
            return false;
        }
        const auto request = accepted_.front();
        accepted_.pop_front();
        completions_.push_back({request.parent_request_id, request.payload_index,
                                ugdr::worker::DatagramResult::success});
        ++completed_tasks_;
        return true;
    }

    void reset_count() noexcept {
        completed_tasks_ = 0;
    }

    [[nodiscard]] std::uint64_t completed_tasks() const noexcept {
        return completed_tasks_;
    }

  private:
    std::size_t capacity_ = 0;
    std::deque<ugdr::worker::BackendRequest> accepted_;
    std::deque<ugdr::worker::BackendCompletion> completions_;
    std::uint64_t completed_tasks_ = 0;
};

class BenchmarkObserver final : public ugdr::worker::ParentCompletionObserver {
  public:
    void record_post(std::uint64_t wr_id, Clock::time_point timestamp) {
        starts_.emplace(wr_id, timestamp);
    }

    void on_parent_completion(const ugdr::worker::ParentCompletionEvent &event) noexcept override {
        const auto start = starts_.find(event.wr_id);
        if (start == starts_.end()) {
            valid_ = false;
            return;
        }
        const double latency =
            std::chrono::duration<double, std::micro>(Clock::now() - start->second).count();
        starts_.erase(start);
        latencies_us_.push_back(latency);
        ++completed_parents_;
        completed_payloads_ += event.payload_count;
        logical_bytes_ += event.logical_bytes;
        valid_ = valid_ && event.result == ugdr::worker::DatagramResult::success;
    }

    void reset() {
        starts_.clear();
        latencies_us_.clear();
        completed_parents_ = 0;
        completed_payloads_ = 0;
        logical_bytes_ = 0;
        valid_ = true;
    }

    [[nodiscard]] bool valid() const noexcept {
        return valid_ && starts_.empty();
    }

    [[nodiscard]] std::uint64_t completed_parents() const noexcept {
        return completed_parents_;
    }

    [[nodiscard]] std::uint64_t completed_payloads() const noexcept {
        return completed_payloads_;
    }

    [[nodiscard]] std::uint64_t logical_bytes() const noexcept {
        return logical_bytes_;
    }

    std::vector<double> sorted_latencies() const {
        auto result = latencies_us_;
        std::sort(result.begin(), result.end());
        return result;
    }

  private:
    std::unordered_map<std::uint64_t, Clock::time_point> starts_;
    std::vector<double> latencies_us_;
    std::uint64_t completed_parents_ = 0;
    std::uint64_t completed_payloads_ = 0;
    std::uint64_t logical_bytes_ = 0;
    bool valid_ = true;
};

struct Endpoint {
    ugdr::ipc::SessionId session = 0;
    std::uint64_t pd_identity = 0;
    std::uint64_t cq_identity = 0;
    std::uint64_t qp_identity = 0;
    ugdr::gpu::ExportedCudaMemory memory;
    ugdr::control::MrRegistrationResult registration;
    std::uint32_t qp_num = 0;
};

bool make_endpoint(ugdr::control::QpService &service, ugdr::ipc::SessionId session,
                   std::uint64_t client_address, std::uint32_t queue_depth, std::uint32_t max_sge,
                   Endpoint *endpoint) {
    endpoint->session = session;
    endpoint->memory.gpu_uuid[0] = 9;
    endpoint->memory.client_address = client_address;
    endpoint->memory.allocation_size = UINT64_C(8) * 1024 * 1024;
    endpoint->memory.length = endpoint->memory.allocation_size;
    endpoint->memory.ipc_handle.resize(64, std::byte{0x37});

    auto context = service.handle(session, decoded(ugdr::control::make_create_context_request(1)));
    auto pd = service.handle(
        session, decoded(ugdr::control::make_create_pd_request(context.response.object_identity)));
    auto cq = service.handle(session, decoded(ugdr::control::make_create_cq_request(
                                          context.response.object_identity, queue_depth * 2)));
    auto mr = service.handle(
        session, decoded(ugdr::control::make_register_mr_request(
                     pd.response.object_identity, endpoint->memory,
                     ugdr::control::kAccessLocalWrite | ugdr::control::kAccessRemoteWrite)));
    ugdr::control::QpCreateAttributes attributes;
    attributes.send_cq_identity = cq.response.object_identity;
    attributes.recv_cq_identity = cq.response.object_identity;
    attributes.max_send_wr = queue_depth;
    attributes.max_recv_wr = queue_depth;
    attributes.max_send_sge = max_sge;
    attributes.max_recv_sge = max_sge;
    attributes.qp_type = ugdr::control::kQpTypeRc;
    auto qp = service.handle(session, decoded(ugdr::control::make_create_qp_request(
                                          pd.response.object_identity, attributes)));
    if (context.response.status != 0 || pd.response.status != 0 || cq.response.status != 0 ||
        mr.response.status != 0 || qp.response.status != 0 ||
        ugdr::control::decode_mr_registration_result(mr.response.opaque, &endpoint->registration) !=
            0) {
        return false;
    }
    endpoint->pd_identity = pd.response.object_identity;
    endpoint->cq_identity = cq.response.object_identity;
    endpoint->qp_identity = qp.response.object_identity;
    for (std::uint32_t qp_num = 1; qp_num != UINT32_MAX; ++qp_num) {
        ugdr::control::WorkerQpView view;
        if (service.worker_qp_view(qp_num, &view) == 0 && view.session_id == session) {
            endpoint->qp_num = qp_num;
            return true;
        }
    }
    return false;
}

bool connect_endpoints(ugdr::control::QpService &service, const Endpoint &first,
                       const Endpoint &second) {
    ugdr::control::QpAttributes init;
    init.state = ugdr::control::kQpStateInit;
    init.current_state = ugdr::control::kQpStateReset;
    init.access_flags = ugdr::control::kQpAccessRemoteWrite;
    constexpr std::uint32_t init_mask = ugdr::control::kQpMaskState |
                                        ugdr::control::kQpMaskCurrentState |
                                        ugdr::control::kQpMaskAccess;
    if (service.handle(first.session, decoded(ugdr::control::make_modify_qp_request(
                                          first.qp_identity, init, init_mask)))
                .response.status != 0 ||
        service.handle(second.session, decoded(ugdr::control::make_modify_qp_request(
                                           second.qp_identity, init, init_mask)))
                .response.status != 0) {
        return false;
    }
    ugdr::control::QpAttributes retry;
    retry.timeout = 1;
    retry.retry_count = 1;
    retry.rnr_retry = 1;
    retry.min_rnr_timer = 1;
    return service.handle(first.session, decoded(ugdr::control::make_connect_qp_request(
                                             first.qp_identity, second.qp_num, retry,
                                             ugdr::control::kQpConnectMask)))
                   .response.status == 0 &&
           service.handle(second.session, decoded(ugdr::control::make_connect_qp_request(
                                              second.qp_identity, first.qp_num, retry,
                                              ugdr::control::kQpConnectMask)))
                   .response.status == 0;
}

struct BenchmarkCase {
    std::uint32_t wr_bytes = 0;
    std::uint32_t payload_bytes = 0;
    std::uint32_t sge_count = 0;
    std::uint32_t queue_depth = 0;
    std::uint32_t signaling_interval = 0;
    std::uint64_t warmup = 0;
    std::uint64_t iterations = 0;
};

std::uint64_t payloads_per_wr(const BenchmarkCase &parameters) {
    const std::uint32_t base = parameters.wr_bytes / parameters.sge_count;
    const std::uint32_t remainder = parameters.wr_bytes % parameters.sge_count;
    std::uint64_t count = 0;
    for (std::uint32_t index = 0; index < parameters.sge_count; ++index) {
        const std::uint32_t length = base + (index < remainder ? 1 : 0);
        count += (static_cast<std::uint64_t>(length) + parameters.payload_bytes - 1) /
                 parameters.payload_bytes;
    }
    return count;
}

bool drain_cq(ugdr::queue::SharedRing &cq) {
    bool progressed = false;
    while (true) {
        ugdr::queue::ConstSlotBatch batch;
        const int status = cq.consumer_peek(cq.descriptor().capacity, &batch);
        if (status == EAGAIN) {
            return progressed;
        }
        if (status != 0 || batch.count == 0 || cq.consumer_release(batch.count) != 0) {
            return false;
        }
        progressed = true;
    }
}

bool post_one(const BenchmarkCase &parameters, const Endpoint &source, const Endpoint &target,
              ugdr::queue::SharedRing &send_queue, std::uint32_t max_send_sge,
              std::uint64_t wr_id) {
    std::vector<ugdr_sge> sges(parameters.sge_count);
    const std::uint32_t base = parameters.wr_bytes / parameters.sge_count;
    const std::uint32_t remainder = parameters.wr_bytes % parameters.sge_count;
    std::uint64_t offset = 0;
    for (std::uint32_t index = 0; index < parameters.sge_count; ++index) {
        const std::uint32_t length = base + (index < remainder ? 1 : 0);
        sges[index] = {source.memory.client_address + offset, length, source.registration.lkey};
        offset += length;
    }
    ugdr_send_wr wr{};
    wr.wr_id = wr_id;
    wr.sg_list = sges.data();
    wr.num_sge = static_cast<int>(sges.size());
    wr.opcode = UGDR_WR_RDMA_WRITE;
    if (parameters.signaling_interval != 0 && wr_id % parameters.signaling_interval == 0) {
        wr.send_flags = UGDR_SEND_SIGNALED;
    }
    wr.wr.rdma.remote_addr = target.memory.client_address;
    wr.wr.rdma.rkey = target.registration.rkey;
    ugdr_send_wr *bad_wr = nullptr;
    return ugdr::api::post_send_chain(send_queue, max_send_sge, &wr, &bad_wr) == 0 &&
           bad_wr == nullptr;
}

bool run_phase(const BenchmarkCase &parameters, std::uint64_t iterations, std::uint64_t *next_wr_id,
               const Endpoint &source, const Endpoint &target,
               ugdr::control::WorkerQpView &requester_view, ugdr::worker::LoopWorker &requester,
               ugdr::worker::LoopWorker &responder, ControlOnlyBackend &backend,
               BenchmarkObserver &observer, double *elapsed_seconds) {
    observer.reset();
    backend.reset_count();
    std::uint64_t posted = 0;
    const auto start = Clock::now();
    while (observer.completed_parents() != iterations) {
        while (posted != iterations &&
               posted - observer.completed_parents() < parameters.queue_depth) {
            const std::uint64_t wr_id = (*next_wr_id)++;
            observer.record_post(wr_id, Clock::now());
            if (!post_one(parameters, source, target, *requester_view.send_queue,
                          requester_view.max_send_sge, wr_id)) {
                return false;
            }
            ++posted;
        }
        bool progressed = requester.progress_once();
        progressed = responder.progress_once() || progressed;
        progressed = backend.progress_once() || progressed;
        progressed = responder.progress_once() || progressed;
        progressed = requester.progress_once() || progressed;
        progressed = drain_cq(*requester_view.send_cq) || progressed;
        if (!progressed) {
            return false;
        }
    }
    *elapsed_seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return observer.valid();
}

double percentile(const std::vector<double> &sorted, double quantile) {
    if (sorted.empty()) {
        return 0.0;
    }
    const std::size_t index =
        static_cast<std::size_t>(quantile * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

bool run(const BenchmarkCase &parameters) {
    FakeCudaMemoryBackend memory_backend;
    ugdr::control::QpService service(memory_backend);
    Endpoint requester_endpoint;
    Endpoint responder_endpoint;
    if (!make_endpoint(service, 601, UINT64_C(0x100000000), parameters.queue_depth,
                       parameters.sge_count, &requester_endpoint) ||
        !make_endpoint(service, 602, UINT64_C(0x200000000), parameters.queue_depth,
                       parameters.sge_count, &responder_endpoint) ||
        !connect_endpoints(service, requester_endpoint, responder_endpoint)) {
        return false;
    }

    const std::size_t backend_capacity =
        static_cast<std::size_t>(parameters.queue_depth) * payloads_per_wr(parameters);
    ugdr::worker::LocalTransport transport(parameters.queue_depth, parameters.queue_depth);
    ControlOnlyBackend backend(std::max<std::size_t>(backend_capacity, 1));
    BenchmarkObserver observer;
    ugdr::worker::LoopWorker requester(service, requester_endpoint.qp_num, transport, backend,
                                       ugdr::worker::LoopWorkerRole::requester,
                                       parameters.payload_bytes, &observer);
    ugdr::worker::LoopWorker responder(service, responder_endpoint.qp_num, transport, backend,
                                       ugdr::worker::LoopWorkerRole::responder,
                                       parameters.payload_bytes);
    ugdr::control::WorkerQpView requester_view;
    if (service.worker_qp_view(requester_endpoint.qp_num, &requester_view) != 0) {
        return false;
    }

    std::uint64_t next_wr_id = 1;
    double warmup_seconds = 0.0;
    if (!run_phase(parameters, parameters.warmup, &next_wr_id, requester_endpoint,
                   responder_endpoint, requester_view, requester, responder, backend, observer,
                   &warmup_seconds)) {
        return false;
    }
    double elapsed_seconds = 0.0;
    if (!run_phase(parameters, parameters.iterations, &next_wr_id, requester_endpoint,
                   responder_endpoint, requester_view, requester, responder, backend, observer,
                   &elapsed_seconds)) {
        return false;
    }

    const std::uint64_t expected_payloads = parameters.iterations * payloads_per_wr(parameters);
    if (observer.completed_parents() != parameters.iterations ||
        observer.completed_payloads() != expected_payloads ||
        backend.completed_tasks() != expected_payloads ||
        observer.logical_bytes() != parameters.iterations * parameters.wr_bytes ||
        elapsed_seconds <= 0.0) {
        return false;
    }
    const auto latencies = observer.sorted_latencies();
    const double parent_mwr =
        static_cast<double>(observer.completed_parents()) / elapsed_seconds / 1'000'000.0;
    const double payload_mtask =
        static_cast<double>(backend.completed_tasks()) / elapsed_seconds / 1'000'000.0;
    const double logical_gb =
        static_cast<double>(observer.logical_bytes()) / elapsed_seconds / 1'000'000'000.0;
    const double p50 = percentile(latencies, 0.50);
    const double p99 = percentile(latencies, 0.99);
    if (!std::isfinite(parent_mwr) || !std::isfinite(payload_mtask) || !std::isfinite(logical_gb) ||
        !std::isfinite(p50) || !std::isfinite(p99)) {
        return false;
    }

    std::printf("benchmark=loop_worker_payload build_type=%s cpu_threads=%u wr_bytes=%u "
                "payload_bytes=%u sge_count=%u queue_depth=%u signaling_interval=%u warmup=%llu "
                "iterations=%llu completed_parent_wr=%llu completed_payload_tasks=%llu "
                "logical_payload_bytes=%llu parent_MWR_per_s=%.6f payload_MTask_per_s=%.6f "
                "logical_payload_GB_per_s=%.6f wr_p50_us=%.3f wr_p99_us=%.3f latency_samples=%zu\n",
                UGDR_BENCHMARK_BUILD_TYPE, std::thread::hardware_concurrency(), parameters.wr_bytes,
                parameters.payload_bytes, parameters.sge_count, parameters.queue_depth,
                parameters.signaling_interval, static_cast<unsigned long long>(parameters.warmup),
                static_cast<unsigned long long>(parameters.iterations),
                static_cast<unsigned long long>(observer.completed_parents()),
                static_cast<unsigned long long>(backend.completed_tasks()),
                static_cast<unsigned long long>(observer.logical_bytes()), parent_mwr,
                payload_mtask, logical_gb, p50, p99, latencies.size());
    return true;
}

}  // namespace

int main() {
    constexpr std::array cases{
        BenchmarkCase{4096, 8192, 1, 32, 1, 1000, 10000},
        BenchmarkCase{65536, 8192, 1, 64, 32, 1000, 10000},
        BenchmarkCase{65536, 8192, 4, 64, 32, 1000, 10000},
        BenchmarkCase{65536, 4096, 4, 64, 0, 1000, 10000},
    };
    for (const auto &parameters : cases) {
        if (!run(parameters)) {
            return 1;
        }
    }
    return 0;
}

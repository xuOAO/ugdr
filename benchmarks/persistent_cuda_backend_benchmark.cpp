#include "api/wr_posting.hpp"
#include "control/qp.hpp"
#include "gpu/gpudirect_visibility.hpp"
#include "gpu/persistent_copy.hpp"
#include "gpu/persistent_copy_backend.hpp"
#include "worker/worker.hpp"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kWrBytes = 64 * 1024;
constexpr std::uint32_t kSinglePayloadWrBytes = 8 * 1024;
constexpr std::uint32_t kPayloadBytes = 8 * 1024;
constexpr std::uint32_t kSgeCount = 4;
constexpr std::uint32_t kBandwidthQueueDepth = 64;
constexpr std::uint32_t kBandwidthSignalingInterval = 32;
constexpr std::uint32_t kLatencyQueueDepth = 1;
constexpr std::uint32_t kLatencySignalingInterval = 1;
constexpr std::uint64_t kWarmupIterations = 1000;
constexpr std::uint64_t kMeasuredIterations = 10000;
constexpr std::uint64_t kDefaultMaxBatchDelayNanoseconds = 50'000;
constexpr std::size_t kPayloadsPerWr = kWrBytes / kPayloadBytes;
constexpr std::size_t kBackendCapacity = kBandwidthQueueDepth * kPayloadsPerWr;
constexpr std::uint64_t kPayloadSeed = UINT64_C(0xf0603007);

ugdr::control::DecodedControlRequest decoded(ugdr::control::UgdrControlRequest request) {
    ugdr::control::DecodedControlRequest value;
    value.value = std::move(request);
    return value;
}

class DirectCudaMemoryBackend final : public ugdr::gpu::CudaIpcMemoryBackend {
  public:
    int open(const ugdr::gpu::ExportedCudaMemory &memory,
             ugdr::gpu::CudaIpcMapping *mapping) override {
        if (mapping == nullptr || memory.client_address == 0 || memory.length == 0) {
            return EINVAL;
        }
        mapping->gpu_uuid = memory.gpu_uuid;
        mapping->daemon_base_address = memory.client_address;
        return 0;
    }

    int close(const ugdr::gpu::CudaIpcMapping &) noexcept override {
        return 0;
    }
};

class BenchmarkObserver final : public ugdr::worker::ParentCompletionObserver {
  public:
    void record_post(std::uint64_t wr_id) {
        if (measure_latency_) {
            starts_.emplace(wr_id, Clock::now());
        }
    }

    void on_parent_completion(const ugdr::worker::ParentCompletionEvent &event) noexcept override {
        if (measure_latency_) {
            const auto start = starts_.find(event.wr_id);
            if (start == starts_.end()) {
                valid_ = false;
                return;
            }
            latencies_us_.push_back(
                std::chrono::duration<double, std::micro>(Clock::now() - start->second).count());
            starts_.erase(start);
        }
        ++completed_parents_;
        completed_payloads_ += event.payload_count;
        logical_bytes_ += event.logical_bytes;
        valid_ = valid_ && event.result == ugdr::worker::DatagramResult::success;
    }

    void reset(bool measure_latency) {
        starts_.clear();
        latencies_us_.clear();
        completed_parents_ = 0;
        completed_payloads_ = 0;
        logical_bytes_ = 0;
        valid_ = true;
        measure_latency_ = measure_latency;
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
    bool measure_latency_ = false;
};

struct Endpoint {
    ugdr::ipc::SessionId session = 0;
    std::uint64_t cq_identity = 0;
    std::uint64_t qp_identity = 0;
    ugdr::gpu::ExportedCudaMemory memory;
    ugdr::control::MrRegistrationResult registration;
    std::uint32_t qp_num = 0;
};

struct HostPhaseProfile {
    std::uint64_t loop_iterations = 0;
    double post_seconds = 0.0;
    double requester_first_seconds = 0.0;
    double responder_seconds = 0.0;
    double requester_second_seconds = 0.0;
    double cq_drain_seconds = 0.0;
};

bool make_endpoint(ugdr::control::QpService &service, ugdr::ipc::SessionId session,
                   std::uint64_t device_address, std::uint32_t access, Endpoint *endpoint) {
    endpoint->session = session;
    endpoint->memory.gpu_uuid[0] = 10;
    endpoint->memory.client_address = device_address;
    endpoint->memory.allocation_size = kWrBytes;
    endpoint->memory.length = kWrBytes;
    endpoint->memory.ipc_handle.resize(64, std::byte{0x37});

    auto context = service.handle(session, decoded(ugdr::control::make_create_context_request(1)));
    auto pd = service.handle(
        session, decoded(ugdr::control::make_create_pd_request(context.response.object_identity)));
    auto cq =
        service.handle(session, decoded(ugdr::control::make_create_cq_request(
                                    context.response.object_identity, kBandwidthQueueDepth * 2)));
    auto mr = service.handle(session, decoded(ugdr::control::make_register_mr_request(
                                          pd.response.object_identity, endpoint->memory, access)));
    ugdr::control::QpCreateAttributes attributes;
    attributes.send_cq_identity = cq.response.object_identity;
    attributes.recv_cq_identity = cq.response.object_identity;
    attributes.max_send_wr = kBandwidthQueueDepth;
    attributes.max_recv_wr = kBandwidthQueueDepth;
    attributes.max_send_sge = kSgeCount;
    attributes.max_recv_sge = kSgeCount;
    attributes.qp_type = ugdr::control::kQpTypeRc;
    auto qp = service.handle(session, decoded(ugdr::control::make_create_qp_request(
                                          pd.response.object_identity, attributes)));
    if (context.response.status != 0 || pd.response.status != 0 || cq.response.status != 0 ||
        mr.response.status != 0 || qp.response.status != 0 ||
        ugdr::control::decode_mr_registration_result(mr.response.opaque, &endpoint->registration) !=
            0) {
        return false;
    }
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

bool drain_cq(ugdr::queue::SharedRing &cq) {
    while (true) {
        ugdr::queue::ConstSlotBatch batch;
        const int status = cq.consumer_peek(cq.descriptor().capacity, &batch);
        if (status == EAGAIN) {
            return true;
        }
        if (status != 0 || batch.count == 0 || cq.consumer_release(batch.count) != 0) {
            return false;
        }
    }
}

bool post_one(const Endpoint &source, const Endpoint &target, ugdr::queue::SharedRing &send_queue,
              std::uint32_t max_send_sge, std::uint64_t wr_id, std::uint32_t wr_bytes,
              std::uint32_t signaling_interval) {
    std::array<ugdr_sge, kSgeCount> sges{};
    const std::uint32_t sge_count = wr_bytes == kSinglePayloadWrBytes ? 1 : kSgeCount;
    const std::uint32_t sge_bytes = wr_bytes / sge_count;
    for (std::uint32_t index = 0; index < sge_count; ++index) {
        sges[index] = {source.memory.client_address + index * sge_bytes, sge_bytes,
                       source.registration.lkey};
    }
    ugdr_send_wr wr{};
    wr.wr_id = wr_id;
    wr.sg_list = sges.data();
    wr.num_sge = static_cast<int>(sge_count);
    wr.opcode = UGDR_WR_RDMA_WRITE;
    if (wr_id % signaling_interval == 0) {
        wr.send_flags = UGDR_SEND_SIGNALED;
    }
    wr.wr.rdma.remote_addr = target.memory.client_address;
    wr.wr.rdma.rkey = target.registration.rkey;
    ugdr_send_wr *bad_wr = nullptr;
    return ugdr::api::post_send_chain(send_queue, max_send_sge, &wr, &bad_wr) == 0 &&
           bad_wr == nullptr;
}

bool run_phase(std::uint64_t iterations, std::uint64_t *next_wr_id, const Endpoint &source,
               const Endpoint &target, ugdr::control::WorkerQpView &requester_view,
               ugdr::worker::LoopWorker &requester, ugdr::worker::LoopWorker &responder,
               BenchmarkObserver &observer, std::uint32_t queue_depth,
               std::uint32_t signaling_interval, std::uint32_t wr_bytes, bool measure_latency,
               double *elapsed_seconds, HostPhaseProfile *profile = nullptr) {
    observer.reset(measure_latency);
    std::uint64_t posted = 0;
    std::uint64_t idle_iterations = 0;
    const auto start = Clock::now();
    while (observer.completed_parents() != iterations) {
        if (profile != nullptr) {
            ++profile->loop_iterations;
        }
        bool progressed = false;
        const auto post_start = profile == nullptr ? Clock::time_point{} : Clock::now();
        while (posted != iterations && posted - observer.completed_parents() < queue_depth) {
            const std::uint64_t wr_id = (*next_wr_id)++;
            observer.record_post(wr_id);
            if (!post_one(source, target, *requester_view.send_queue, requester_view.max_send_sge,
                          wr_id, wr_bytes, signaling_interval)) {
                return false;
            }
            ++posted;
            progressed = true;
        }
        if (profile != nullptr) {
            profile->post_seconds +=
                std::chrono::duration<double>(Clock::now() - post_start).count();
        }
        const auto requester_first_start = profile == nullptr ? Clock::time_point{} : Clock::now();
        progressed = requester.progress_once() || progressed;
        if (profile != nullptr) {
            profile->requester_first_seconds +=
                std::chrono::duration<double>(Clock::now() - requester_first_start).count();
        }
        const auto responder_start = profile == nullptr ? Clock::time_point{} : Clock::now();
        progressed = responder.progress_once() || progressed;
        progressed = responder.progress_once() || progressed;
        if (profile != nullptr) {
            profile->responder_seconds +=
                std::chrono::duration<double>(Clock::now() - responder_start).count();
        }
        const auto requester_second_start = profile == nullptr ? Clock::time_point{} : Clock::now();
        progressed = requester.progress_once() || progressed;
        if (profile != nullptr) {
            profile->requester_second_seconds +=
                std::chrono::duration<double>(Clock::now() - requester_second_start).count();
        }
        const auto cq_drain_start = profile == nullptr ? Clock::time_point{} : Clock::now();
        const bool cq_drained = drain_cq(*requester_view.send_cq);
        if (profile != nullptr) {
            profile->cq_drain_seconds +=
                std::chrono::duration<double>(Clock::now() - cq_drain_start).count();
        }
        if (!cq_drained) {
            return false;
        }
        if (progressed) {
            idle_iterations = 0;
        } else if (++idle_iterations == 10'000'000) {
            return false;
        } else {
            std::this_thread::yield();
        }
    }
    *elapsed_seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return observer.valid();
}

double percentile(const std::vector<double> &sorted, double quantile) {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto index = static_cast<std::size_t>(quantile * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

struct LatencyResult {
    std::uint32_t wr_bytes = 0;
    double warmup_seconds = 0.0;
    double elapsed_seconds = 0.0;
    std::uint64_t completed_parents = 0;
    std::uint64_t completed_payloads = 0;
    std::uint64_t logical_bytes = 0;
    std::vector<double> samples;
    bool ran = false;
};

bool stop_backend(ugdr::gpu::PersistentCudaCopyBackend &backend) {
    if (backend.request_stop() != 0) {
        return false;
    }
    while (backend.wait() == EAGAIN) {
        ugdr::worker::BackendCompletion completion;
        while (backend.try_pop_completion(completion)) {
        }
        std::this_thread::yield();
    }
    return backend.state() == ugdr::gpu::PersistentCudaCopyBackendState::stopped &&
           backend.last_error() == 0;
}

int run(std::uint64_t max_batch_delay_nanoseconds, std::uint64_t bandwidth_iterations,
        bool profile_host) {
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
        return 77;
    }
    cudaDeviceProp properties{};
    int runtime_version = 0;
    int driver_version = 0;
    if (cudaGetDeviceProperties(&properties, 0) != cudaSuccess ||
        cudaRuntimeGetVersion(&runtime_version) != cudaSuccess ||
        cudaDriverGetVersion(&driver_version) != cudaSuccess) {
        return 1;
    }
    ugdr::gpu::GpuDirectVisibilityCapabilities capabilities;
    if (ugdr::gpu::query_gpudirect_visibility_capabilities(0, &capabilities) != 0) {
        return 2;
    }

    ugdr::gpu::PersistentCopyPayloadBuffer payload;
    if (ugdr::gpu::PersistentCopyPayloadBuffer::allocate(kWrBytes, 64, &payload) != 0 ||
        payload.prepare(kPayloadSeed) != 0) {
        return 3;
    }
    DirectCudaMemoryBackend memory_backend;
    ugdr::control::QpService service(memory_backend);
    Endpoint requester_endpoint;
    Endpoint responder_endpoint;
    if (!make_endpoint(service, 601, payload.stage_buffer_base(), ugdr::control::kAccessLocalWrite,
                       &requester_endpoint) ||
        !make_endpoint(service, 602, payload.target_address(),
                       ugdr::control::kAccessLocalWrite | ugdr::control::kAccessRemoteWrite,
                       &responder_endpoint) ||
        !connect_endpoints(service, requester_endpoint, responder_endpoint)) {
        return 4;
    }

    ugdr::gpu::PersistentCudaCopyBackendConfig config;
    config.device_ordinal = 0;
    config.stage_buffer_base = payload.stage_buffer_base();
    config.stage_buffer_bytes = kWrBytes;
    config.queue_capacity = kBackendCapacity;
    config.host_batch = ugdr::gpu::kPersistentCudaCopyBackendHostBatch;
    config.max_batch_delay_nanoseconds = max_batch_delay_nanoseconds;
    ugdr::gpu::PersistentCudaCopyBackend backend;
    if (backend.start(config) != 0) {
        return 5;
    }

    ugdr::worker::LocalTransport transport(kBandwidthQueueDepth, kBandwidthQueueDepth);
    BenchmarkObserver observer;
    ugdr::worker::LoopWorker requester(service, requester_endpoint.qp_num, transport, backend,
                                       ugdr::worker::LoopWorkerRole::requester, kPayloadBytes,
                                       &observer);
    ugdr::worker::LoopWorker responder(service, responder_endpoint.qp_num, transport, backend,
                                       ugdr::worker::LoopWorkerRole::responder, kPayloadBytes);
    ugdr::control::WorkerQpView requester_view;
    if (service.worker_qp_view(requester_endpoint.qp_num, &requester_view) != 0) {
        return 6;
    }

    std::uint64_t next_wr_id = 1;
    double bandwidth_warmup_seconds = 0.0;
    double bandwidth_seconds = 0.0;
    HostPhaseProfile bandwidth_profile;
    const bool bandwidth_ran =
        run_phase(kWarmupIterations, &next_wr_id, requester_endpoint, responder_endpoint,
                  requester_view, requester, responder, observer, kBandwidthQueueDepth,
                  kBandwidthSignalingInterval, kWrBytes, false, &bandwidth_warmup_seconds) &&
        run_phase(bandwidth_iterations, &next_wr_id, requester_endpoint, responder_endpoint,
                  requester_view, requester, responder, observer, kBandwidthQueueDepth,
                  kBandwidthSignalingInterval, kWrBytes, false, &bandwidth_seconds,
                  profile_host ? &bandwidth_profile : nullptr);
    const std::uint64_t bandwidth_completed_parents = observer.completed_parents();
    const std::uint64_t bandwidth_completed_payloads = observer.completed_payloads();
    const std::uint64_t bandwidth_logical_bytes = observer.logical_bytes();

    auto run_latency = [&](std::uint32_t wr_bytes) {
        LatencyResult result;
        result.wr_bytes = wr_bytes;
        result.ran =
            bandwidth_ran &&
            run_phase(kWarmupIterations, &next_wr_id, requester_endpoint, responder_endpoint,
                      requester_view, requester, responder, observer, kLatencyQueueDepth,
                      kLatencySignalingInterval, wr_bytes, false, &result.warmup_seconds) &&
            run_phase(kMeasuredIterations, &next_wr_id, requester_endpoint, responder_endpoint,
                      requester_view, requester, responder, observer, kLatencyQueueDepth,
                      kLatencySignalingInterval, wr_bytes, true, &result.elapsed_seconds);
        result.completed_parents = observer.completed_parents();
        result.completed_payloads = observer.completed_payloads();
        result.logical_bytes = observer.logical_bytes();
        result.samples = observer.sorted_latencies();
        return result;
    };
    const LatencyResult latency_8k = run_latency(kSinglePayloadWrBytes);
    const LatencyResult latency_64k = run_latency(kWrBytes);
    if (!stop_backend(backend)) {
        return 7;
    }

    ugdr::gpu::PayloadCheck check;
    const auto latency_valid = [](const LatencyResult &result) {
        const std::uint64_t payloads_per_wr = (result.wr_bytes + kPayloadBytes - 1) / kPayloadBytes;
        return result.ran && result.completed_parents == kMeasuredIterations &&
               result.completed_payloads == kMeasuredIterations * payloads_per_wr &&
               result.logical_bytes == kMeasuredIterations * result.wr_bytes &&
               result.elapsed_seconds > 0.0 && result.samples.size() == kMeasuredIterations;
    };
    if (!latency_valid(latency_8k) || !latency_valid(latency_64k) ||
        payload.verify(kPayloadSeed, &check) != 0 || !check.payload_matches ||
        !check.guards_intact || bandwidth_completed_parents != bandwidth_iterations ||
        bandwidth_completed_payloads != bandwidth_iterations * kPayloadsPerWr ||
        bandwidth_logical_bytes != bandwidth_iterations * kWrBytes || bandwidth_seconds <= 0.0) {
        return 8;
    }

    const double parent_mwr =
        static_cast<double>(bandwidth_completed_parents) / bandwidth_seconds / 1'000'000.0;
    const double payload_mtask =
        static_cast<double>(bandwidth_completed_payloads) / bandwidth_seconds / 1'000'000.0;
    const double logical_gb =
        static_cast<double>(bandwidth_logical_bytes) / bandwidth_seconds / 1'000'000'000.0;
    if (!std::isfinite(parent_mwr) || !std::isfinite(payload_mtask) || !std::isfinite(logical_gb)) {
        return 9;
    }

    std::printf("benchmark=persistent_cuda_backend_e2e phase=bandwidth build_type=%s "
                "gpu_name=\"%s\" "
                "cuda_runtime=%d cuda_driver=%d gpudirect_rdma=%u writes_ordering=%d "
                "host_flush=%u stream_memops_flush=%u copy_warps=%u device_batch=%u "
                "shared_queue_depth=%u host_batch=%zu backend_queue_capacity=%zu wr_bytes=%u "
                "payload_bytes=%u sge_count=%u queue_depth=%u signaling_interval=%u "
                "max_batch_delay_ns=%llu warmup=%llu "
                "iterations=%llu warmup_seconds=%.6f completed_parent_wr=%llu "
                "completed_payload_tasks=%llu logical_payload_bytes=%llu elapsed_seconds=%.6f "
                "parent_MWR_per_s=%.6f payload_MTask_per_s=%.6f logical_payload_GB_per_s=%.6f "
                "correctness_passed=1\n",
                UGDR_BENCHMARK_BUILD_TYPE, properties.name, runtime_version, driver_version,
                capabilities.gpudirect_rdma_supported ? 1U : 0U,
                static_cast<int>(capabilities.writes_ordering),
                capabilities.host_flush_supported ? 1U : 0U,
                capabilities.stream_memops_flush_supported ? 1U : 0U,
                ugdr::gpu::kPersistentCudaCopyBackendCopyWarps,
                ugdr::gpu::kPersistentCudaCopyBackendDeviceBatch,
                ugdr::gpu::kPersistentCudaCopyBackendSharedQueueDepth, config.host_batch,
                config.queue_capacity, kWrBytes, kPayloadBytes, kSgeCount, kBandwidthQueueDepth,
                kBandwidthSignalingInterval,
                static_cast<unsigned long long>(config.max_batch_delay_nanoseconds),
                static_cast<unsigned long long>(kWarmupIterations),
                static_cast<unsigned long long>(bandwidth_iterations), bandwidth_warmup_seconds,
                static_cast<unsigned long long>(bandwidth_completed_parents),
                static_cast<unsigned long long>(bandwidth_completed_payloads),
                static_cast<unsigned long long>(bandwidth_logical_bytes), bandwidth_seconds,
                parent_mwr, payload_mtask, logical_gb);
    if (profile_host) {
        const double accounted_seconds =
            bandwidth_profile.post_seconds + bandwidth_profile.requester_first_seconds +
            bandwidth_profile.responder_seconds + bandwidth_profile.requester_second_seconds +
            bandwidth_profile.cq_drain_seconds;
        const double unaccounted_seconds = std::max(0.0, bandwidth_seconds - accounted_seconds);
        const auto share = [bandwidth_seconds](double seconds) {
            return seconds / bandwidth_seconds * 100.0;
        };
        std::printf(
            "benchmark=persistent_cuda_backend_e2e phase=host_profile "
            "loop_iterations=%llu elapsed_seconds=%.6f post_seconds=%.6f "
            "post_percent=%.2f requester_first_seconds=%.6f "
            "requester_first_percent=%.2f responder_seconds=%.6f "
            "responder_percent=%.2f requester_second_seconds=%.6f "
            "requester_second_percent=%.2f cq_drain_seconds=%.6f "
            "cq_drain_percent=%.2f unaccounted_seconds=%.6f "
            "unaccounted_percent=%.2f\n",
            static_cast<unsigned long long>(bandwidth_profile.loop_iterations), bandwidth_seconds,
            bandwidth_profile.post_seconds, share(bandwidth_profile.post_seconds),
            bandwidth_profile.requester_first_seconds,
            share(bandwidth_profile.requester_first_seconds), bandwidth_profile.responder_seconds,
            share(bandwidth_profile.responder_seconds), bandwidth_profile.requester_second_seconds,
            share(bandwidth_profile.requester_second_seconds), bandwidth_profile.cq_drain_seconds,
            share(bandwidth_profile.cq_drain_seconds), unaccounted_seconds,
            share(unaccounted_seconds));
    }
    for (const LatencyResult *result : {&latency_8k, &latency_64k}) {
        const double minimum = result->samples.front();
        const double maximum = result->samples.back();
        const double average =
            std::accumulate(result->samples.begin(), result->samples.end(), 0.0) /
            result->samples.size();
        const double p50 = percentile(result->samples, 0.50);
        const double p99 = percentile(result->samples, 0.99);
        const double p999 = percentile(result->samples, 0.999);
        if (!std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(average) ||
            !std::isfinite(p50) || !std::isfinite(p99) || !std::isfinite(p999)) {
            return 9;
        }
        const std::size_t payloads_per_wr = (result->wr_bytes + kPayloadBytes - 1) / kPayloadBytes;
        std::printf("benchmark=persistent_cuda_backend_e2e phase=latency build_type=%s "
                    "gpu_name=\"%s\" wr_bytes=%u payload_bytes=%u payloads_per_wr=%zu sge_count=%u "
                    "queue_depth=%u signaling_interval=%u max_batch_delay_ns=%llu warmup=%llu "
                    "iterations=%llu "
                    "warmup_seconds=%.6f elapsed_seconds=%.6f samples=%zu latency_min_us=%.3f "
                    "latency_max_us=%.3f latency_avg_us=%.3f latency_p50_us=%.3f "
                    "latency_p99_us=%.3f latency_p99_9_us=%.3f correctness_passed=1\n",
                    UGDR_BENCHMARK_BUILD_TYPE, properties.name, result->wr_bytes, kPayloadBytes,
                    payloads_per_wr, result->wr_bytes == kSinglePayloadWrBytes ? 1U : kSgeCount,
                    kLatencyQueueDepth, kLatencySignalingInterval,
                    static_cast<unsigned long long>(config.max_batch_delay_nanoseconds),
                    static_cast<unsigned long long>(kWarmupIterations),
                    static_cast<unsigned long long>(kMeasuredIterations), result->warmup_seconds,
                    result->elapsed_seconds, result->samples.size(), minimum, maximum, average, p50,
                    p99, p999);
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    std::uint64_t max_batch_delay_nanoseconds = kDefaultMaxBatchDelayNanoseconds;
    std::uint64_t bandwidth_iterations = kMeasuredIterations;
    bool profile_host = false;
    bool delay_set = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--profile-host") == 0) {
            profile_host = true;
            continue;
        }
        if (std::strcmp(argv[index], "--bandwidth-iterations") == 0) {
            if (++index == argc) {
                std::fprintf(stderr, "missing value for --bandwidth-iterations\n");
                return 64;
            }
            char *end = nullptr;
            errno = 0;
            const unsigned long long parsed = std::strtoull(argv[index], &end, 10);
            if (errno != 0 || end == argv[index] || *end != '\0' || parsed == 0) {
                std::fprintf(stderr, "invalid bandwidth iterations: %s\n", argv[index]);
                return 64;
            }
            bandwidth_iterations = parsed;
            continue;
        }
        if (delay_set) {
            std::fprintf(stderr, "duplicate max_batch_delay_ns: %s\n", argv[index]);
            return 64;
        }
        char *end = nullptr;
        errno = 0;
        const unsigned long long parsed = std::strtoull(argv[index], &end, 10);
        if (errno != 0 || end == argv[index] || *end != '\0' || parsed == 0) {
            std::fprintf(stderr, "invalid max_batch_delay_ns: %s\n", argv[index]);
            return 64;
        }
        max_batch_delay_nanoseconds = parsed;
        delay_set = true;
    }
    return run(max_batch_delay_nanoseconds, bandwidth_iterations, profile_host);
}

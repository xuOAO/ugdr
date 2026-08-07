#include "control/qp.hpp"
#include "gpu/cuda_ipc_memory.hpp"
#include "gpu/persistent_copy_backend.hpp"
#include "ipc/ipc.hpp"
#include "ugdr/api.hpp"
#include "worker/local_transport.hpp"
#include "worker/worker.hpp"

#include <cuda_runtime_api.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

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
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kPayloadBytes = 8 * 1024;
constexpr std::uint32_t kSinglePayloadWrBytes = 8 * 1024;
constexpr std::uint32_t kLatencyLargeWrBytes = 64 * 1024;
constexpr std::uint32_t kMaxWrBytes = 64 * 1024 * 1024;
constexpr std::size_t kQueueDepth = 64;
constexpr std::size_t kRequestCapacity = 512;
constexpr std::size_t kResponseCapacity = kQueueDepth;
constexpr std::size_t kBackendCapacity = 1024;
constexpr std::uint64_t kDefaultWarmupBytes = UINT64_C(512) * 1024 * 1024;
constexpr std::uint64_t kDefaultMeasuredBytes = UINT64_C(4) * 1024 * 1024 * 1024;
constexpr std::uint64_t kDefaultLatencyWarmup = 100;
constexpr std::uint64_t kDefaultLatencyIterations = 5000;
constexpr std::uint64_t kDefaultMaxBatchDelayNanoseconds = 50'000;
constexpr std::chrono::seconds kCompletionTimeout{30};
constexpr std::uint64_t kControlPollInterval = 1024;
constexpr std::uint64_t kDeadlineCheckInterval = UINT64_C(1) << 20U;
constexpr std::array<std::uint32_t, 14> kBandwidthWrSizes{
    8 * 1024,        16 * 1024,        32 * 1024,        64 * 1024,       128 * 1024,
    256 * 1024,      512 * 1024,       1024 * 1024,      2 * 1024 * 1024, 4 * 1024 * 1024,
    8 * 1024 * 1024, 16 * 1024 * 1024, 32 * 1024 * 1024, 64 * 1024 * 1024};

struct Options {
    std::uint64_t warmup_batches = 0;
    std::uint64_t bandwidth_batches = 0;
    std::uint64_t latency_warmup = kDefaultLatencyWarmup;
    std::uint64_t latency_iterations = kDefaultLatencyIterations;
    std::uint64_t max_batch_delay_nanoseconds = kDefaultMaxBatchDelayNanoseconds;
    std::uint32_t wr_bytes = 0;
};

struct BandwidthResult {
    std::uint32_t wr_bytes = 0;
    std::uint64_t warmup_batches = 0;
    std::uint64_t measured_batches = 0;
    double warmup_seconds = 0.0;
    double elapsed_seconds = 0.0;
};

struct LatencyResult {
    std::uint32_t wr_bytes = 0;
    double elapsed_seconds = 0.0;
    std::vector<double> samples_us;
};

class ClientDaemonDataService final : public ugdr::control::ControlService {
  public:
    explicit ClientDaemonDataService(std::uint64_t max_batch_delay_nanoseconds)
        : transport_(kRequestCapacity, kResponseCapacity),
          max_batch_delay_nanoseconds_(max_batch_delay_nanoseconds) {
    }

    ugdr::control::ControlServiceResult
    handle(ugdr::ipc::SessionId session_id, ugdr::control::DecodedControlRequest request) override {
        const auto method = static_cast<ugdr::control::ControlMethod>(request.value.method);
        ugdr::gpu::ExportedCudaMemory source_registration;
        const bool registering_stage_buffer =
            method == ugdr::control::ControlMethod::register_mr &&
            request.value.access == ugdr::control::kAccessLocalWrite &&
            ugdr::control::decode_mr_registration(request.value.opaque, request.value.length,
                                                  &source_registration) == 0;
        const std::uint64_t registration_pd = request.value.object_identity;
        auto result = service_.handle(session_id, std::move(request));
        if (result.response.status == 0 && registering_stage_buffer) {
            ugdr::control::MrRegistrationResult registration_result;
            std::uint64_t daemon_address = 0;
            if (ugdr::control::decode_mr_registration_result(result.response.opaque,
                                                             &registration_result) != 0 ||
                service_.resolve_lkey(session_id, registration_pd, registration_result.lkey,
                                      source_registration.client_address,
                                      source_registration.length, &daemon_address) != 0) {
                backend_status_ = EPROTO;
            } else {
                stage_buffer_base_ = daemon_address;
                stage_buffer_bytes_ = source_registration.length;
            }
        }
        if (result.response.status == 0 && method == ugdr::control::ControlMethod::connect_qp) {
            ++connected_qps_;
            make_workers_if_ready();
        }
        return result;
    }

    void on_disconnect(ugdr::ipc::SessionId session_id) noexcept override {
        service_.on_disconnect(session_id);
    }

    void progress() {
        if (service_.qp_count() == 0) {
            progress_backend_shutdown();
            return;
        }
        if (!workers_ready()) {
            return;
        }
        (void)requester_->progress_once();
        (void)responder_->progress_once();
        (void)responder_->progress_once();
        (void)requester_->progress_once();
    }

    [[nodiscard]] bool workers_ready() const noexcept {
        return requester_ != nullptr && responder_ != nullptr;
    }

    [[nodiscard]] bool finished() const noexcept {
        return backend_status_ == 0 &&
               backend_.state() == ugdr::gpu::PersistentCudaCopyBackendState::stopped &&
               workers_ready() && service_.qp_count() == 0 && service_.cq_count() == 0 &&
               service_.mr_count() == 0 && service_.pd_count() == 0 &&
               service_.context_count() == 0;
    }

    [[nodiscard]] int status() const noexcept {
        return backend_status_;
    }

    ugdr::gpu::RuntimeCudaIpcMemoryBackend memory_backend_;

  private:
    void make_workers_if_ready() {
        if (connected_qps_ < 2 || workers_ready()) {
            return;
        }
        std::array<std::uint32_t, 2> qp_nums{};
        std::size_t found = 0;
        for (std::uint32_t qp_num = 1; found < qp_nums.size() && qp_num < 32; ++qp_num) {
            ugdr::control::WorkerQpView view;
            if (service_.worker_qp_view(qp_num, &view) == 0) {
                qp_nums[found++] = qp_num;
            }
        }
        if (found != qp_nums.size()) {
            return;
        }
        if (stage_buffer_base_ == 0 || stage_buffer_bytes_ < kMaxWrBytes) {
            backend_status_ = EINVAL;
            return;
        }
        ugdr::gpu::PersistentCudaCopyBackendConfig config;
        config.device_ordinal = 0;
        config.stage_buffer_base = stage_buffer_base_;
        config.stage_buffer_bytes = stage_buffer_bytes_;
        config.queue_capacity = kBackendCapacity;
        config.host_batch = ugdr::gpu::kPersistentCudaCopyBackendHostBatch;
        config.max_batch_delay_nanoseconds = max_batch_delay_nanoseconds_;
        backend_status_ = backend_.start(config);
        if (backend_status_ != 0) {
            return;
        }
        requester_ = std::make_unique<ugdr::worker::LoopWorker>(
            service_, qp_nums[0], transport_, backend_, ugdr::worker::LoopWorkerRole::requester,
            kPayloadBytes);
        responder_ = std::make_unique<ugdr::worker::LoopWorker>(
            service_, qp_nums[1], transport_, backend_, ugdr::worker::LoopWorkerRole::responder,
            kPayloadBytes);
    }

    void progress_backend_shutdown() {
        if (backend_.state() == ugdr::gpu::PersistentCudaCopyBackendState::accepting) {
            backend_status_ = backend_.request_stop();
            if (backend_status_ != 0) {
                return;
            }
        }
        if (backend_.state() != ugdr::gpu::PersistentCudaCopyBackendState::draining) {
            return;
        }
        std::array<ugdr::worker::BackendCompletion, ugdr::gpu::kPersistentCudaCopyBackendHostBatch>
            completions{};
        while (backend_.try_pop_completion_batch(completions.data(), completions.size()) != 0) {
        }
        const int status = backend_.wait();
        if (status != 0 && status != EAGAIN) {
            backend_status_ = status;
        }
    }

    ugdr::control::QpService service_{memory_backend_};
    ugdr::worker::LocalTransport transport_;
    ugdr::gpu::PersistentCudaCopyBackend backend_;
    std::uint64_t max_batch_delay_nanoseconds_ = 0;
    int connected_qps_ = 0;
    int backend_status_ = 0;
    std::uint64_t stage_buffer_base_ = 0;
    std::uint64_t stage_buffer_bytes_ = 0;
    std::unique_ptr<ugdr::worker::LoopWorker> requester_;
    std::unique_ptr<ugdr::worker::LoopWorker> responder_;
};

int child_main(const std::string &socket_path, int ready_fd,
               std::uint64_t max_batch_delay_nanoseconds) {
    ClientDaemonDataService data(max_batch_delay_nanoseconds);
    if (data.memory_backend_.initialization_status() != 0) {
        const char skipped = 's';
        if (::write(ready_fd, &skipped, 1) != 1) {
            return 19;
        }
        return 77;
    }
    ugdr::control::ControlIpcHandler handler(data);
    ugdr::ipc::IpcServer server(handler);
    if (server.start(socket_path) != 0) {
        return 20;
    }
    const char ready = 'r';
    if (::write(ready_fd, &ready, 1) != 1) {
        return 21;
    }

    std::uint64_t progress_cycles = 0;
    const auto deadline = Clock::now() + std::chrono::minutes(2);
    while (true) {
        const bool poll_control =
            !data.workers_ready() || (progress_cycles & (kControlPollInterval - 1)) == 0;
        if (poll_control && server.poll_once(data.workers_ready() ? 0 : 10) != 0) {
            return 22;
        }
        data.progress();
        if (data.status() != 0) {
            return 23;
        }
        if (data.finished()) {
            return 0;
        }
        ++progress_cycles;
        if ((progress_cycles & (kDeadlineCheckInterval - 1)) == 0 && Clock::now() >= deadline) {
            return 24;
        }
    }
}

int initialize(ugdr_qp *qp) {
    ugdr_qp_attr attributes{};
    attributes.qp_state = UGDR_QPS_INIT;
    attributes.cur_qp_state = UGDR_QPS_RESET;
    attributes.qp_access_flags = UGDR_ACCESS_REMOTE_WRITE;
    return ugdr_modify_qp(qp, &attributes,
                          UGDR_QP_STATE | UGDR_QP_CUR_STATE | UGDR_QP_ACCESS_FLAGS);
}

int connect(ugdr_qp *qp, std::uint32_t remote_qp_num) {
    const ugdr_qp_conn_info remote{remote_qp_num};
    ugdr_qp_attr retry{};
    retry.timeout = 1;
    retry.retry_cnt = 1;
    retry.rnr_retry = 1;
    retry.min_rnr_timer = 1;
    constexpr int mask =
        UGDR_QP_TIMEOUT | UGDR_QP_RETRY_CNT | UGDR_QP_RNR_RETRY | UGDR_QP_MIN_RNR_TIMER;
    return ugdr_connect_qp(qp, &remote, &retry, mask);
}

class SendBatch {
  public:
    SendBatch(void *source, std::uint32_t lkey, void *target, std::uint32_t rkey)
        : source_(reinterpret_cast<std::uint64_t>(source)), lkey_(lkey),
          target_(reinterpret_cast<std::uint64_t>(target)), rkey_(rkey) {
    }

    ugdr_send_wr *prepare(std::uint64_t first_wr_id, std::size_t count, std::uint32_t wr_bytes) {
        for (std::size_t index = 0; index < count; ++index) {
            sges_[index] = {source_, wr_bytes, lkey_};
            ugdr_send_wr &wr = wrs_[index];
            wr = {};
            wr.wr_id = first_wr_id + index;
            wr.next = index + 1 == count ? nullptr : &wrs_[index + 1];
            wr.sg_list = &sges_[index];
            wr.num_sge = 1;
            wr.opcode = UGDR_WR_RDMA_WRITE;
            wr.send_flags = index + 1 == count ? UGDR_SEND_SIGNALED : 0;
            wr.wr.rdma.remote_addr = target_;
            wr.wr.rdma.rkey = rkey_;
        }
        return wrs_.data();
    }

  private:
    std::uint64_t source_ = 0;
    std::uint32_t lkey_ = 0;
    std::uint64_t target_ = 0;
    std::uint32_t rkey_ = 0;
    std::array<ugdr_sge, kQueueDepth> sges_{};
    std::array<ugdr_send_wr, kQueueDepth> wrs_{};
};

bool poll_completion(ugdr_cq *cq, std::uint64_t expected_wr_id) {
    const auto deadline = Clock::now() + kCompletionTimeout;
    std::uint64_t empty_polls = 0;
    while (true) {
        ugdr_wc completion{};
        const int count = ugdr_poll_cq(cq, 1, &completion);
        if (count < 0) {
            return false;
        }
        if (count == 1) {
            return completion.wr_id == expected_wr_id && completion.status == UGDR_WC_SUCCESS &&
                   completion.opcode == UGDR_WC_RDMA_WRITE;
        }
        ++empty_polls;
        if ((empty_polls & (kDeadlineCheckInterval - 1)) == 0 && Clock::now() >= deadline) {
            return false;
        }
    }
}

bool run_bandwidth_batches(ugdr_qp *qp, ugdr_cq *cq, SendBatch &batch, std::uint32_t wr_bytes,
                           std::uint64_t batch_count, std::uint64_t *next_wr_id,
                           double *elapsed_seconds) {
    const auto start = Clock::now();
    for (std::uint64_t batch_index = 0; batch_index < batch_count; ++batch_index) {
        const std::uint64_t first_wr_id = *next_wr_id;
        ugdr_send_wr *const first = batch.prepare(first_wr_id, kQueueDepth, wr_bytes);
        ugdr_send_wr *bad = nullptr;
        if (ugdr_post_send(qp, first, &bad) != 0 || bad != nullptr ||
            !poll_completion(cq, first_wr_id + kQueueDepth - 1)) {
            return false;
        }
        *next_wr_id += kQueueDepth;
    }
    *elapsed_seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return true;
}

std::uint64_t batches_for_target_bytes(std::uint64_t target_bytes, std::uint32_t wr_bytes) {
    const std::uint64_t bytes_per_batch = static_cast<std::uint64_t>(wr_bytes) * kQueueDepth;
    return std::max(UINT64_C(1), (target_bytes + bytes_per_batch - 1) / bytes_per_batch);
}

bool run_latency(ugdr_qp *qp, ugdr_cq *cq, SendBatch &batch, std::uint32_t wr_bytes,
                 std::uint64_t warmup, std::uint64_t iterations, std::uint64_t *next_wr_id,
                 LatencyResult *result) {
    result->wr_bytes = wr_bytes;
    result->samples_us.clear();
    result->samples_us.reserve(iterations);
    const std::uint64_t total = warmup + iterations;
    const auto phase_start = Clock::now();
    for (std::uint64_t index = 0; index < total; ++index) {
        const std::uint64_t wr_id = (*next_wr_id)++;
        ugdr_send_wr *const wr = batch.prepare(wr_id, 1, wr_bytes);
        ugdr_send_wr *bad = nullptr;
        const auto start = Clock::now();
        if (ugdr_post_send(qp, wr, &bad) != 0 || bad != nullptr || !poll_completion(cq, wr_id)) {
            return false;
        }
        if (index >= warmup) {
            result->samples_us.push_back(
                std::chrono::duration<double, std::micro>(Clock::now() - start).count());
        }
    }
    result->elapsed_seconds = std::chrono::duration<double>(Clock::now() - phase_start).count();
    std::sort(result->samples_us.begin(), result->samples_us.end());
    return result->samples_us.size() == iterations;
}

double percentile(const std::vector<double> &sorted, double quantile) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double position = quantile * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1, sorted.size() - 1);
    const double fraction = position - static_cast<double>(lower);
    return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
}

bool parse_u64(const char *value, std::uint64_t *parsed) {
    if (value == nullptr || value[0] == '\0' || parsed == nullptr) {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long result = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *parsed = result;
    return true;
}

bool parse_options(int argc, char **argv, Options *options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help") {
            std::printf("usage: %s [--warmup-batches N] [--bandwidth-batches N] "
                        "[--latency-warmup N] [--latency-iterations N] "
                        "[--max-batch-delay-ns N] [--wr-bytes N]\n",
                        argv[0]);
            return false;
        }
        if (index + 1 >= argc) {
            return false;
        }
        std::uint64_t value = 0;
        if (!parse_u64(argv[++index], &value)) {
            return false;
        }
        if (argument == "--warmup-batches") {
            options->warmup_batches = value;
        } else if (argument == "--bandwidth-batches") {
            options->bandwidth_batches = value;
        } else if (argument == "--latency-warmup") {
            options->latency_warmup = value;
        } else if (argument == "--latency-iterations") {
            options->latency_iterations = value;
        } else if (argument == "--max-batch-delay-ns") {
            options->max_batch_delay_nanoseconds = value;
        } else if (argument == "--wr-bytes") {
            if (value < kPayloadBytes || value > kMaxWrBytes || value % kPayloadBytes != 0) {
                return false;
            }
            options->wr_bytes = static_cast<std::uint32_t>(value);
        } else {
            return false;
        }
    }
    return options->latency_iterations != 0;
}

void terminate_process(pid_t *process) {
    if (*process <= 0) {
        return;
    }
    (void)::kill(*process, SIGTERM);
    (void)::waitpid(*process, nullptr, 0);
    *process = -1;
}

int run_benchmark(const Options &options) {
    char directory_template[] = "/tmp/ugdr-client-daemon-cuda-XXXXXX";
    char *const directory = ::mkdtemp(directory_template);
    if (directory == nullptr) {
        return 1;
    }
    const std::string socket_path = std::string(directory) + "/control.sock";
    std::array<int, 2> ready_pipe{};
    if (::pipe2(ready_pipe.data(), O_CLOEXEC) != 0) {
        (void)::rmdir(directory);
        return 2;
    }
    pid_t child = ::fork();
    if (child < 0) {
        (void)::close(ready_pipe[0]);
        (void)::close(ready_pipe[1]);
        (void)::rmdir(directory);
        return 3;
    }
    if (child == 0) {
        (void)::close(ready_pipe[0]);
        const int result =
            child_main(socket_path, ready_pipe[1], options.max_batch_delay_nanoseconds);
        (void)::close(ready_pipe[1]);
        std::_Exit(result);
    }
    (void)::close(ready_pipe[1]);
    char ready = 0;
    if (::read(ready_pipe[0], &ready, 1) != 1) {
        (void)::close(ready_pipe[0]);
        terminate_process(&child);
        (void)::rmdir(directory);
        return 4;
    }
    (void)::close(ready_pipe[0]);
    if (ready == 's') {
        int child_status = 0;
        (void)::waitpid(child, &child_status, 0);
        (void)::rmdir(directory);
        return 77;
    }
    if (ready != 'r' || ::setenv("UGDR_DAEMON_SOCKET", socket_path.c_str(), 1) != 0) {
        terminate_process(&child);
        (void)::rmdir(directory);
        return 5;
    }

    int result = 0;
    void *source = nullptr;
    void *target = nullptr;
    ugdr_context *context = nullptr;
    ugdr_pd *source_pd = nullptr;
    ugdr_pd *target_pd = nullptr;
    ugdr_mr *source_mr = nullptr;
    ugdr_mr *target_mr = nullptr;
    ugdr_cq *send_cq = nullptr;
    ugdr_cq *receive_cq = nullptr;
    ugdr_qp *requester = nullptr;
    ugdr_qp *responder = nullptr;
    std::vector<unsigned char> source_data(kMaxWrBytes);
    std::vector<unsigned char> observed(kMaxWrBytes);
    for (std::size_t index = 0; index < source_data.size(); ++index) {
        source_data[index] = static_cast<unsigned char>((index * 17 + 29) & 0xffU);
    }

    std::vector<BandwidthResult> bandwidth_results;
    LatencyResult latency_8k;
    LatencyResult latency_64k;
    cudaDeviceProp properties{};
    int runtime_version = 0;
    int driver_version = 0;
    do {
        if (cudaGetDeviceProperties(&properties, 0) != cudaSuccess ||
            cudaRuntimeGetVersion(&runtime_version) != cudaSuccess ||
            cudaDriverGetVersion(&driver_version) != cudaSuccess ||
            cudaMalloc(&source, kMaxWrBytes) != cudaSuccess ||
            cudaMalloc(&target, kMaxWrBytes) != cudaSuccess ||
            cudaMemcpy(source, source_data.data(), source_data.size(), cudaMemcpyHostToDevice) !=
                cudaSuccess ||
            cudaMemset(target, 0, kMaxWrBytes) != cudaSuccess) {
            result = 77;
            break;
        }
        int device_count = 0;
        ugdr_device **devices = ugdr_get_device_list(&device_count);
        context = devices != nullptr && device_count == 1 ? ugdr_open_device(devices[0]) : nullptr;
        if (devices != nullptr) {
            ugdr_free_device_list(devices);
        }
        source_pd = context == nullptr ? nullptr : ugdr_alloc_pd(context);
        target_pd = context == nullptr ? nullptr : ugdr_alloc_pd(context);
        source_mr = source_pd == nullptr
                        ? nullptr
                        : ugdr_reg_mr(source_pd, source, kMaxWrBytes, UGDR_ACCESS_LOCAL_WRITE);
        target_mr = target_pd == nullptr
                        ? nullptr
                        : ugdr_reg_mr(target_pd, target, kMaxWrBytes,
                                      UGDR_ACCESS_LOCAL_WRITE | UGDR_ACCESS_REMOTE_WRITE);
        send_cq = context == nullptr
                      ? nullptr
                      : ugdr_create_cq(context, static_cast<int>(kQueueDepth), nullptr, nullptr, 0);
        receive_cq = context == nullptr ? nullptr : ugdr_create_cq(context, 1, nullptr, nullptr, 0);
        ugdr_qp_init_attr requester_attributes{
            send_cq, send_cq, static_cast<std::uint32_t>(kQueueDepth), 1, 1, 1, UGDR_QPT_RC, 0};
        ugdr_qp_init_attr responder_attributes{receive_cq, receive_cq, 1, 1, 1, 1, UGDR_QPT_RC, 0};
        requester =
            source_pd == nullptr ? nullptr : ugdr_create_qp(source_pd, &requester_attributes);
        responder =
            target_pd == nullptr ? nullptr : ugdr_create_qp(target_pd, &responder_attributes);
        ugdr_qp_conn_info requester_info{};
        ugdr_qp_conn_info responder_info{};
        if (source_mr == nullptr || target_mr == nullptr || send_cq == nullptr ||
            receive_cq == nullptr || requester == nullptr || responder == nullptr ||
            initialize(requester) != 0 || initialize(responder) != 0 ||
            ugdr_query_qp_conn_info(requester, &requester_info) != 0 ||
            ugdr_query_qp_conn_info(responder, &responder_info) != 0 ||
            connect(requester, responder_info.qp_num) != 0 ||
            connect(responder, requester_info.qp_num) != 0) {
            result = 6;
            break;
        }

        SendBatch batch(source, source_mr->lkey, target, target_mr->rkey);
        std::uint64_t next_wr_id = 1;
        const auto run_bandwidth_case = [&](std::uint32_t wr_bytes) {
            BandwidthResult measured;
            measured.wr_bytes = wr_bytes;
            measured.warmup_batches = options.warmup_batches == 0
                                          ? batches_for_target_bytes(kDefaultWarmupBytes, wr_bytes)
                                          : options.warmup_batches;
            measured.measured_batches =
                options.bandwidth_batches == 0
                    ? batches_for_target_bytes(kDefaultMeasuredBytes, wr_bytes)
                    : options.bandwidth_batches;
            const bool passed =
                run_bandwidth_batches(requester, send_cq, batch, wr_bytes, measured.warmup_batches,
                                      &next_wr_id, &measured.warmup_seconds) &&
                run_bandwidth_batches(requester, send_cq, batch, wr_bytes,
                                      measured.measured_batches, &next_wr_id,
                                      &measured.elapsed_seconds);
            bandwidth_results.push_back(measured);
            return passed;
        };
        bool bandwidth_passed = true;
        if (options.wr_bytes != 0) {
            bandwidth_passed = run_bandwidth_case(options.wr_bytes);
        } else {
            for (const std::uint32_t wr_bytes : kBandwidthWrSizes) {
                if (!run_bandwidth_case(wr_bytes)) {
                    bandwidth_passed = false;
                    break;
                }
            }
        }
        const std::uint32_t verification_bytes =
            options.wr_bytes == 0 ? kMaxWrBytes : std::max(options.wr_bytes, kLatencyLargeWrBytes);
        if (!bandwidth_passed ||
            !run_latency(requester, send_cq, batch, kSinglePayloadWrBytes, options.latency_warmup,
                         options.latency_iterations, &next_wr_id, &latency_8k) ||
            !run_latency(requester, send_cq, batch, kLatencyLargeWrBytes, options.latency_warmup,
                         options.latency_iterations, &next_wr_id, &latency_64k) ||
            cudaMemcpy(observed.data(), target, verification_bytes, cudaMemcpyDeviceToHost) !=
                cudaSuccess ||
            !std::equal(source_data.begin(), source_data.begin() + verification_bytes,
                        observed.begin())) {
            result = 7;
            break;
        }

        for (const BandwidthResult &bandwidth : bandwidth_results) {
            const std::uint64_t completed_wr = bandwidth.measured_batches * kQueueDepth;
            const std::uint64_t payloads_per_wr = bandwidth.wr_bytes / kPayloadBytes;
            const std::uint64_t completed_payloads = completed_wr * payloads_per_wr;
            const std::uint64_t logical_bytes = completed_wr * bandwidth.wr_bytes;
            const double parent_mwr =
                static_cast<double>(completed_wr) / bandwidth.elapsed_seconds / 1'000'000.0;
            const double payload_mtask =
                static_cast<double>(completed_payloads) / bandwidth.elapsed_seconds / 1'000'000.0;
            const double logical_gb =
                static_cast<double>(logical_bytes) / bandwidth.elapsed_seconds / 1'000'000'000.0;
            if (!std::isfinite(parent_mwr) || !std::isfinite(payload_mtask) ||
                !std::isfinite(logical_gb) || bandwidth.elapsed_seconds <= 0.0) {
                result = 8;
                break;
            }
            std::printf(
                "benchmark=client_daemon_cuda_e2e phase=bandwidth "
                "boundary=post_send_to_poll_cq build_type=%s gpu_name=\"%s\" cuda_runtime=%d "
                "cuda_driver=%d l2_cache_bytes=%d wr_bytes=%u payload_bytes=%u "
                "payloads_per_wr=%llu "
                "queue_depth=%zu post_chain_length=%zu sq_consume_slots=1 cqe_per_batch=1 "
                "host_batch=%zu backend_queue_capacity=%zu control_poll_interval=%llu "
                "max_batch_delay_ns=%llu warmup_batches=%llu measured_batches=%llu "
                "completed_parent_wr=%llu completed_payload_tasks=%llu "
                "logical_payload_bytes=%llu warmup_seconds=%.6f elapsed_seconds=%.6f "
                "parent_MWR_per_s=%.6f payload_MTask_per_s=%.6f "
                "logical_payload_GB_per_s=%.6f correctness_passed=1\n",
                UGDR_BENCHMARK_BUILD_TYPE, properties.name, runtime_version, driver_version,
                properties.l2CacheSize, bandwidth.wr_bytes, kPayloadBytes,
                static_cast<unsigned long long>(payloads_per_wr), kQueueDepth, kQueueDepth,
                ugdr::gpu::kPersistentCudaCopyBackendHostBatch, kBackendCapacity,
                static_cast<unsigned long long>(kControlPollInterval),
                static_cast<unsigned long long>(options.max_batch_delay_nanoseconds),
                static_cast<unsigned long long>(bandwidth.warmup_batches),
                static_cast<unsigned long long>(bandwidth.measured_batches),
                static_cast<unsigned long long>(completed_wr),
                static_cast<unsigned long long>(completed_payloads),
                static_cast<unsigned long long>(logical_bytes), bandwidth.warmup_seconds,
                bandwidth.elapsed_seconds, parent_mwr, payload_mtask, logical_gb);
        }
        if (result != 0) {
            break;
        }

        for (const LatencyResult *latency : {&latency_8k, &latency_64k}) {
            const double average =
                std::accumulate(latency->samples_us.begin(), latency->samples_us.end(), 0.0) /
                latency->samples_us.size();
            std::printf(
                "benchmark=client_daemon_cuda_e2e phase=latency boundary=post_send_to_poll_cq "
                "build_type=%s gpu_name=\"%s\" wr_bytes=%u payload_bytes=%u "
                "payloads_per_wr=%u queue_depth=1 post_chain_length=1 sq_consume_slots=1 "
                "cqe_per_wr=1 max_batch_delay_ns=%llu warmup=%llu iterations=%llu "
                "elapsed_seconds=%.6f samples=%zu latency_min_us=%.3f latency_max_us=%.3f "
                "latency_avg_us=%.3f latency_p50_us=%.3f latency_p99_us=%.3f "
                "latency_p99_9_us=%.3f correctness_passed=1\n",
                UGDR_BENCHMARK_BUILD_TYPE, properties.name, latency->wr_bytes, kPayloadBytes,
                (latency->wr_bytes + kPayloadBytes - 1) / kPayloadBytes,
                static_cast<unsigned long long>(options.max_batch_delay_nanoseconds),
                static_cast<unsigned long long>(options.latency_warmup),
                static_cast<unsigned long long>(options.latency_iterations),
                latency->elapsed_seconds, latency->samples_us.size(), latency->samples_us.front(),
                latency->samples_us.back(), average, percentile(latency->samples_us, 0.50),
                percentile(latency->samples_us, 0.99), percentile(latency->samples_us, 0.999));
        }
    } while (false);

    bool cleanup_ok = true;
    if (requester != nullptr) {
        cleanup_ok = ugdr_destroy_qp(requester) == 0 && cleanup_ok;
    }
    if (responder != nullptr) {
        cleanup_ok = ugdr_destroy_qp(responder) == 0 && cleanup_ok;
    }
    if (send_cq != nullptr) {
        cleanup_ok = ugdr_destroy_cq(send_cq) == 0 && cleanup_ok;
    }
    if (receive_cq != nullptr) {
        cleanup_ok = ugdr_destroy_cq(receive_cq) == 0 && cleanup_ok;
    }
    if (source_mr != nullptr) {
        cleanup_ok = ugdr_dereg_mr(source_mr) == 0 && cleanup_ok;
    }
    if (target_mr != nullptr) {
        cleanup_ok = ugdr_dereg_mr(target_mr) == 0 && cleanup_ok;
    }
    if (source_pd != nullptr) {
        cleanup_ok = ugdr_dealloc_pd(source_pd) == 0 && cleanup_ok;
    }
    if (target_pd != nullptr) {
        cleanup_ok = ugdr_dealloc_pd(target_pd) == 0 && cleanup_ok;
    }
    if (context != nullptr) {
        cleanup_ok = ugdr_close_device(context) == 0 && cleanup_ok;
    }
    if (source != nullptr) {
        cleanup_ok = cudaFree(source) == cudaSuccess && cleanup_ok;
    }
    if (target != nullptr) {
        cleanup_ok = cudaFree(target) == cudaSuccess && cleanup_ok;
    }
    (void)::unsetenv("UGDR_DAEMON_SOCKET");

    if (result == 0 && cleanup_ok) {
        int child_status = 0;
        if (::waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
            WEXITSTATUS(child_status) != 0) {
            result = 9;
        }
        child = -1;
    } else {
        terminate_process(&child);
        if (result == 0) {
            result = 10;
        }
    }
    (void)::rmdir(directory);
    return result;
}

}  // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        return argc == 2 && std::strcmp(argv[1], "--help") == 0 ? 0 : 2;
    }
    return run_benchmark(options);
}

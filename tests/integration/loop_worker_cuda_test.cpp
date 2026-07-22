#include "control/qp.hpp"
#include "gpu/cuda_ipc_memory.hpp"
#include "ipc/ipc.hpp"
#include "support/loop_worker_fixture.hpp"
#include "ugdr/api.hpp"
#include "worker/local_transport.hpp"
#include "worker/worker.hpp"

#include <cuda_runtime_api.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class LoopDataService final : public ugdr::control::ControlService {
  public:
    LoopDataService() : service(memory_backend), transport(8, 8), backend(8) {
    }

    ugdr::control::ControlServiceResult
    handle(ugdr::ipc::SessionId session_id, ugdr::control::DecodedControlRequest request) override {
        const auto method = static_cast<ugdr::control::ControlMethod>(request.value.method);
        auto result = service.handle(session_id, std::move(request));
        if (result.response.status == 0 && method == ugdr::control::ControlMethod::connect_qp) {
            ++connected_qps;
            make_workers_if_ready();
        }
        return result;
    }

    void on_disconnect(ugdr::ipc::SessionId session_id) noexcept override {
        service.on_disconnect(session_id);
    }

    void progress(std::size_t *tokens) {
        if (requester == nullptr || responder == nullptr || service.qp_count() != 2) {
            return;
        }
        (void)requester->progress_once();
        (void)responder->progress_once();
        if (*tokens != 0 && backend.progress_once()) {
            --*tokens;
        }
        (void)responder->progress_once();
        (void)requester->progress_once();
    }

    [[nodiscard]] bool ready() const noexcept {
        return requester != nullptr && responder != nullptr;
    }

    [[nodiscard]] bool finished() const noexcept {
        return ready() && service.qp_count() == 0 && service.cq_count() == 0 &&
               service.mr_count() == 0 && service.pd_count() == 0 && service.context_count() == 0;
    }

    ugdr::gpu::RuntimeCudaIpcMemoryBackend memory_backend;
    ugdr::control::QpService service;
    ugdr::worker::LocalTransport transport;
    ugdr::test::MockGpuBackend backend;

  private:
    void make_workers_if_ready() {
        if (connected_qps < 2 || requester != nullptr) {
            return;
        }
        std::array<std::uint32_t, 2> qp_nums{};
        std::size_t found = 0;
        for (std::uint32_t qp_num = 1; found < qp_nums.size() && qp_num < 32; ++qp_num) {
            ugdr::control::WorkerQpView view;
            if (service.worker_qp_view(qp_num, &view) == 0) {
                qp_nums[found++] = qp_num;
            }
        }
        if (found != qp_nums.size()) {
            return;
        }
        requester = std::make_unique<ugdr::worker::LoopWorker>(
            service, qp_nums[0], transport, backend, ugdr::worker::LoopWorkerRole::requester);
        responder = std::make_unique<ugdr::worker::LoopWorker>(
            service, qp_nums[1], transport, backend, ugdr::worker::LoopWorkerRole::responder);
    }

    int connected_qps = 0;
    std::unique_ptr<ugdr::worker::LoopWorker> requester;
    std::unique_ptr<ugdr::worker::LoopWorker> responder;
};

int child_main(const std::string &socket_path, int ready_fd, int progress_fd) {
    LoopDataService data;
    if (data.memory_backend.initialization_status() != 0) {
        const char skipped = 's';
        (void)::write(ready_fd, &skipped, 1);
        return 77;
    }
    const int flags = ::fcntl(progress_fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(progress_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return 20;
    }
    ugdr::control::ControlIpcHandler handler(data);
    ugdr::ipc::IpcServer server(handler);
    if (server.start(socket_path) != 0) {
        return 21;
    }
    const char ready = 'r';
    if (::write(ready_fd, &ready, 1) != 1) {
        return 22;
    }

    std::size_t progress_tokens = 0;
    for (int iteration = 0; iteration < 30000; ++iteration) {
        if (server.poll_once(1) != 0) {
            return 23;
        }
        std::array<char, 32> bytes{};
        const ssize_t count = ::read(progress_fd, bytes.data(), bytes.size());
        if (count > 0) {
            progress_tokens += static_cast<std::size_t>(count);
        } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return 24;
        }
        data.progress(&progress_tokens);
        if (data.finished()) {
            return 0;
        }
    }
    return 25;
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

int poll_until(ugdr_cq *cq, ugdr_wc *entry) {
    for (int iteration = 0; iteration < 5000; ++iteration) {
        const int count = ugdr_poll_cq(cq, 1, entry);
        if (count != 0) {
            return count;
        }
        ::usleep(1000);
    }
    return 0;
}

void terminate_process(pid_t *process) {
    if (*process <= 0) {
        return;
    }
    (void)::kill(*process, SIGTERM);
    (void)::waitpid(*process, nullptr, 0);
    *process = -1;
}

}  // namespace

int main() {
    char directory_template[] = "/tmp/ugdr-loop-worker-cuda-XXXXXX";
    char *const directory = ::mkdtemp(directory_template);
    if (directory == nullptr) {
        return 1;
    }
    const std::string socket_path = std::string(directory) + "/control.sock";
    std::array<int, 2> ready_pipe{};
    std::array<int, 2> progress_pipe{};
    if (::pipe2(ready_pipe.data(), O_CLOEXEC) != 0 ||
        ::pipe2(progress_pipe.data(), O_CLOEXEC) != 0) {
        return 2;
    }
    pid_t child = ::fork();
    if (child < 0) {
        return 3;
    }
    if (child == 0) {
        ::close(ready_pipe[0]);
        ::close(progress_pipe[1]);
        const int result = child_main(socket_path, ready_pipe[1], progress_pipe[0]);
        ::close(progress_pipe[0]);
        ::close(ready_pipe[1]);
        std::_Exit(result);
    }
    ::close(ready_pipe[1]);
    ::close(progress_pipe[0]);
    char ready = 0;
    if (::read(ready_pipe[0], &ready, 1) != 1) {
        terminate_process(&child);
        return 4;
    }
    ::close(ready_pipe[0]);
    if (ready == 's') {
        (void)::close(progress_pipe[1]);
        int child_status = 0;
        (void)::waitpid(child, &child_status, 0);
        (void)::rmdir(directory);
        return 77;
    }
    if (ready != 'r' || ::setenv("UGDR_DAEMON_SOCKET", socket_path.c_str(), 1) != 0) {
        terminate_process(&child);
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
    constexpr std::size_t allocation_size = 256;
    std::vector<unsigned char> source_data(allocation_size);
    std::vector<unsigned char> observed(allocation_size);
    for (std::size_t index = 0; index < source_data.size(); ++index) {
        source_data[index] = static_cast<unsigned char>(index + 1);
    }

    do {
        if (cudaMalloc(&source, allocation_size) != cudaSuccess ||
            cudaMalloc(&target, allocation_size) != cudaSuccess ||
            cudaMemcpy(source, source_data.data(), source_data.size(), cudaMemcpyHostToDevice) !=
                cudaSuccess ||
            cudaMemset(target, 0, allocation_size) != cudaSuccess) {
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
                        : ugdr_reg_mr(source_pd, source, allocation_size, UGDR_ACCESS_LOCAL_WRITE);
        target_mr = target_pd == nullptr
                        ? nullptr
                        : ugdr_reg_mr(target_pd, target, allocation_size,
                                      UGDR_ACCESS_LOCAL_WRITE | UGDR_ACCESS_REMOTE_WRITE);
        send_cq = context == nullptr ? nullptr : ugdr_create_cq(context, 8, nullptr, nullptr, 0);
        receive_cq = context == nullptr ? nullptr : ugdr_create_cq(context, 8, nullptr, nullptr, 0);
        ugdr_qp_init_attr requester_attributes{send_cq, send_cq, 8, 8, 4, 4, UGDR_QPT_RC, 0};
        ugdr_qp_init_attr responder_attributes{receive_cq, receive_cq, 8, 8, 4, 4, UGDR_QPT_RC, 0};
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

        ugdr_recv_wr receive{};
        receive.wr_id = 900;
        ugdr_recv_wr *bad_receive = nullptr;
        std::array<ugdr_sge, 2> sges{{
            {static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(source)), 32,
             source_mr->lkey},
            {static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(source)) + 64, 16,
             source_mr->lkey},
        }};
        ugdr_send_wr send{};
        send.wr_id = 800;
        send.sg_list = sges.data();
        send.num_sge = static_cast<int>(sges.size());
        send.opcode = UGDR_WR_RDMA_WRITE_WITH_IMM;
        send.send_flags = UGDR_SEND_SIGNALED;
        send.imm_data = UINT32_C(0x12345678);
        send.wr.rdma.remote_addr =
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(target)) + 32;
        send.wr.rdma.rkey = target_mr->rkey;
        ugdr_send_wr *bad_send = nullptr;
        if (ugdr_post_recv(responder, &receive, &bad_receive) != 0 || bad_receive != nullptr ||
            ugdr_post_send(requester, &send, &bad_send) != 0 || bad_send != nullptr) {
            result = 7;
            break;
        }

        ::usleep(20000);
        ugdr_wc early{};
        if (ugdr_poll_cq(send_cq, 1, &early) != 0 || ugdr_poll_cq(receive_cq, 1, &early) != 0 ||
            cudaMemcpy(observed.data(), target, observed.size(), cudaMemcpyDeviceToHost) !=
                cudaSuccess) {
            result = 8;
            break;
        }
        for (unsigned char value : observed) {
            if (value != 0) {
                result = 9;
                break;
            }
        }
        if (result != 0) {
            break;
        }
        const char progress = 'p';
        if (::write(progress_pipe[1], &progress, 1) != 1) {
            result = 10;
            break;
        }
        ugdr_wc send_completion{};
        ugdr_wc receive_completion{};
        if (poll_until(send_cq, &send_completion) != 1 ||
            poll_until(receive_cq, &receive_completion) != 1 || send_completion.wr_id != 800 ||
            send_completion.status != UGDR_WC_SUCCESS ||
            send_completion.opcode != UGDR_WC_RDMA_WRITE ||
            send_completion.qp_num != requester_info.qp_num || receive_completion.wr_id != 900 ||
            receive_completion.status != UGDR_WC_SUCCESS ||
            receive_completion.opcode != UGDR_WC_RECV_RDMA_WITH_IMM ||
            receive_completion.byte_len != 48 ||
            receive_completion.imm_data != UINT32_C(0x12345678) ||
            receive_completion.qp_num != responder_info.qp_num ||
            receive_completion.wc_flags != UGDR_WC_WITH_IMM ||
            cudaMemcpy(observed.data(), target, observed.size(), cudaMemcpyDeviceToHost) !=
                cudaSuccess) {
            result = 11;
            break;
        }
        for (std::size_t index = 0; index < observed.size(); ++index) {
            unsigned char expected = 0;
            if (index >= 32 && index < 64) {
                expected = source_data[index - 32];
            } else if (index >= 64 && index < 80) {
                expected = source_data[index];
            }
            if (observed[index] != expected) {
                result = 12;
                break;
            }
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
    (void)::close(progress_pipe[1]);
    (void)::unsetenv("UGDR_DAEMON_SOCKET");

    if (result == 0 && cleanup_ok) {
        int child_status = 0;
        if (::waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
            WEXITSTATUS(child_status) != 0) {
            result = 13;
        }
        child = -1;
    } else {
        terminate_process(&child);
        if (result == 0) {
            result = 14;
        }
    }
    (void)::rmdir(directory);
    return result;
}

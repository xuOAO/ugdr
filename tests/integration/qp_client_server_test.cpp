#include "control/qp.hpp"
#include "ugdr/api.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace {

class UnusedCudaBackend final : public ugdr::gpu::CudaIpcMemoryBackend {
  public:
    int open(const ugdr::gpu::ExportedCudaMemory &, ugdr::gpu::CudaIpcMapping *) override {
        return EIO;
    }

    int close(const ugdr::gpu::CudaIpcMapping &) noexcept override {
        return EIO;
    }
};

class CountingService final : public ugdr::control::ControlService {
  public:
    CountingService() : service(backend) {
    }

    ugdr::control::ControlServiceResult
    handle(ugdr::ipc::SessionId session_id, ugdr::control::DecodedControlRequest request) override {
        ++request_count;
        return service.handle(session_id, std::move(request));
    }

    void on_disconnect(ugdr::ipc::SessionId session_id) noexcept override {
        service.on_disconnect(session_id);
    }

    UnusedCudaBackend backend;
    ugdr::control::QpService service;
    int request_count = 0;
};

int child_main(const std::string &socket_path, int ready_fd) {
    CountingService service;
    ugdr::control::ControlIpcHandler handler(service);
    ugdr::ipc::IpcServer server(handler);
    if (server.start(socket_path) != 0) {
        return 20;
    }
    const char ready = 'r';
    if (::write(ready_fd, &ready, 1) != 1) {
        return 21;
    }
    for (int iteration = 0; iteration < 500 && service.request_count < 17; ++iteration) {
        if (server.poll_once(50) != 0) {
            return 22;
        }
    }
    return service.request_count == 17 && service.service.qp_count() == 0 &&
                   service.service.cq_count() == 0 && service.service.pd_count() == 0 &&
                   service.service.context_count() == 0
               ? 0
               : 23;
}

ugdr_qp_init_attr init_attributes(ugdr_cq *send_cq, ugdr_cq *recv_cq) {
    return {send_cq, recv_cq, 17, 19, 3, 5, UGDR_QPT_RC, 1};
}

}  // namespace

int main() {
    char directory_template[] = "/tmp/ugdr-qp-test-XXXXXX";
    char *const directory = ::mkdtemp(directory_template);
    if (directory == nullptr) {
        return 1;
    }
    const std::string socket_path = std::string(directory) + "/control.sock";
    std::array<int, 2> ready_pipe{};
    if (::pipe2(ready_pipe.data(), O_CLOEXEC) != 0) {
        ::rmdir(directory);
        return 2;
    }
    const pid_t child = ::fork();
    if (child < 0) {
        return 3;
    }
    if (child == 0) {
        ::close(ready_pipe[0]);
        const int result = child_main(socket_path, ready_pipe[1]);
        ::close(ready_pipe[1]);
        std::_Exit(result);
    }
    ::close(ready_pipe[1]);
    char ready = 0;
    if (::read(ready_pipe[0], &ready, 1) != 1 || ready != 'r') {
        return 4;
    }
    ::close(ready_pipe[0]);
    if (::setenv("UGDR_DAEMON_SOCKET", socket_path.c_str(), 1) != 0) {
        return 5;
    }

    int count = 0;
    ugdr_device **devices = ugdr_get_device_list(&count);
    if (devices == nullptr || count != 1 || devices[0] == nullptr) {
        return 6;
    }
    ugdr_context *const context = ugdr_open_device(devices[0]);
    ugdr_free_device_list(devices);
    ugdr_pd *const pd = ugdr_alloc_pd(context);
    ugdr_cq *const send_cq = ugdr_create_cq(context, 32, nullptr, nullptr, 0);
    ugdr_cq *const recv_cq = ugdr_create_cq(context, 32, nullptr, nullptr, 0);
    if (context == nullptr || pd == nullptr || send_cq == nullptr || recv_cq == nullptr) {
        return 7;
    }

    ugdr_qp_init_attr invalid = init_attributes(send_cq, recv_cq);
    invalid.max_send_wr = 0;
    const ugdr_qp_init_attr expected_invalid = invalid;
    errno = 0;
    if (ugdr_create_qp(pd, &invalid) != nullptr || errno != EINVAL ||
        std::memcmp(&invalid, &expected_invalid, sizeof(invalid)) != 0) {
        return 8;
    }

    ugdr_qp_init_attr same = init_attributes(send_cq, send_cq);
    const ugdr_qp_init_attr expected_same = same;
    ugdr_qp *const same_qp = ugdr_create_qp(pd, &same);
    if (same_qp == nullptr || std::memcmp(&same, &expected_same, sizeof(same)) != 0 ||
        ugdr_destroy_cq(send_cq) != EBUSY || ugdr_dealloc_pd(pd) != EBUSY ||
        ugdr_destroy_qp(same_qp) != 0 || ugdr_destroy_qp(same_qp) != EINVAL) {
        return 9;
    }

    ugdr_qp_init_attr split = init_attributes(send_cq, recv_cq);
    const ugdr_qp_init_attr expected_split = split;
    ugdr_qp *const split_qp = ugdr_create_qp(pd, &split);
    if (split_qp == nullptr || std::memcmp(&split, &expected_split, sizeof(split)) != 0 ||
        ugdr_destroy_cq(send_cq) != EBUSY || ugdr_destroy_cq(recv_cq) != EBUSY ||
        ugdr_modify_qp(split_qp, nullptr, 0) != EOPNOTSUPP ||
        ugdr_query_qp_conn_info(split_qp, nullptr) != EOPNOTSUPP ||
        ugdr_destroy_qp(split_qp) != 0) {
        return 10;
    }
    if (ugdr_destroy_cq(send_cq) != 0 || ugdr_destroy_cq(recv_cq) != 0 ||
        ugdr_dealloc_pd(pd) != 0 || ugdr_close_device(context) != 0) {
        return 11;
    }

    int child_status = 0;
    if (::waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) {
        return 12;
    }
    return ::rmdir(directory) == 0 ? 0 : 13;
}

#include "control/pd_mr_cq.hpp"
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
    ugdr::control::PdMrCqService service;
    int request_count = 0;
};

int child_main(const std::string &socket_path, int ready_fd) {
    CountingService service;
    ugdr::control::ControlIpcHandler handler(service);
    ugdr::ipc::IpcServer server(handler);
    const int start_status = server.start(socket_path);
    if (start_status != 0) {
        dprintf(STDERR_FILENO, "server start failed: %d\n", start_status);
        return 20;
    }
    const char ready = 'r';
    if (::write(ready_fd, &ready, 1) != 1) {
        return 21;
    }
    for (int iteration = 0; iteration < 300 && service.request_count < 8; ++iteration) {
        if (server.poll_once(50) != 0) {
            return 22;
        }
    }
    return service.request_count == 8 && service.service.context_count() == 0 &&
                   service.service.pd_count() == 0 && service.service.cq_count() == 0
               ? 0
               : 23;
}

}  // namespace

int main() {
    char directory_template[] = "/tmp/ugdr-pd-cq-test-XXXXXX";
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
    ugdr_cq *const cq = ugdr_create_cq(context, 32, reinterpret_cast<void *>(17), nullptr, 0);
    if (context == nullptr || pd == nullptr || cq == nullptr) {
        return 7;
    }
    errno = 0;
    if (ugdr_create_cq(context, 0, nullptr, nullptr, 0) != nullptr || errno != EINVAL) {
        return 8;
    }
    errno = 0;
    if (ugdr_close_device(context) != -1 || errno != EBUSY) {
        return 9;
    }

    ugdr_wc wc{};
    wc.wr_id = 91;
    const ugdr_wc expected = wc;
    if (ugdr_poll_cq(cq, 1, &wc) != 0 || std::memcmp(&wc, &expected, sizeof(wc)) != 0) {
        return 10;
    }
    if (ugdr_destroy_cq(cq) != 0 || ugdr_destroy_cq(cq) != EINVAL || ugdr_dealloc_pd(pd) != 0 ||
        ugdr_dealloc_pd(pd) != EINVAL || ugdr_close_device(context) != 0) {
        return 11;
    }

    int child_status = 0;
    if (::waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) {
        return 12;
    }
    return ::rmdir(directory) == 0 ? 0 : 13;
}

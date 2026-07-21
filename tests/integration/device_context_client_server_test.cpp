#include "control/device_context.hpp"
#include "ugdr/api.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace {

class CountingService final : public ugdr::control::ControlService {
  public:
    ugdr::control::ControlServiceResult
    handle(ugdr::ipc::SessionId session_id, ugdr::control::DecodedControlRequest request) override {
        ++request_count;
        return service.handle(session_id, std::move(request));
    }

    void on_disconnect(ugdr::ipc::SessionId session_id) noexcept override {
        service.on_disconnect(session_id);
    }

    ugdr::control::DeviceContextService service;
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
    for (int iteration = 0; iteration < 200 && service.request_count < 3; ++iteration) {
        if (server.poll_once(50) != 0) {
            return 22;
        }
    }
    return service.request_count == 3 && service.service.context_count() == 0 ? 0 : 23;
}

}  // namespace

int main() {
    char directory_template[] = "/tmp/ugdr-device-context-test-XXXXXX";
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

    int count = 99;
    ugdr_device **devices = ugdr_get_device_list(&count);
    if (devices == nullptr || count != 1 || devices[0] == nullptr || devices[1] != nullptr) {
        return 6;
    }
    ugdr_device *const device = devices[0];
    ugdr_context *const context = ugdr_open_device(device);
    if (context == nullptr) {
        return 7;
    }
    errno = 77;
    ugdr_free_device_list(devices);
    if (errno != 77) {
        return 8;
    }
    errno = 0;
    if (ugdr_open_device(device) != nullptr || errno != EINVAL) {
        return 9;
    }
    if (ugdr_close_device(context) != 0) {
        return 10;
    }
    errno = 0;
    if (ugdr_close_device(context) != -1 || errno != EINVAL) {
        return 11;
    }
    errno = 0;
    ugdr_free_device_list(devices);
    if (errno != EINVAL) {
        return 12;
    }

    int child_status = 0;
    if (::waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) {
        return 13;
    }
    return ::rmdir(directory) == 0 ? 0 : 14;
}

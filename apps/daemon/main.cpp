#include "data_service.hpp"

#include "control/control.hpp"
#include "ipc/ipc.hpp"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

#include <unistd.h>

namespace {

volatile std::sig_atomic_t stop_requested = 0;
constexpr std::uint64_t kDefaultMaxBatchDelayNanoseconds = 50'000;

extern "C" void request_stop(int) {
    stop_requested = 1;
}

int configured_ready_fd() noexcept {
    const char *const value = std::getenv("UGDR_DAEMON_READY_FD");
    if (value == nullptr || value[0] == '\0') {
        return -1;
    }
    errno = 0;
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return errno == 0 && end != value && *end == '\0' && parsed >= 0 &&
                   parsed <= std::numeric_limits<int>::max()
               ? static_cast<int>(parsed)
               : -1;
}

std::uint64_t configured_max_batch_delay() noexcept {
    const char *const value = std::getenv("UGDR_MAX_BATCH_DELAY_NS");
    if (value == nullptr || value[0] == '\0') {
        return kDefaultMaxBatchDelayNanoseconds;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    return errno == 0 && end != value && *end == '\0' && parsed != 0
               ? parsed
               : kDefaultMaxBatchDelayNanoseconds;
}

void notify_ready(int ready_fd, char state) noexcept {
    if (ready_fd >= 0) {
        (void)::write(ready_fd, &state, 1);
    }
}

int run_server(const char *socket_path) {
    const int ready_fd = configured_ready_fd();
    ugdr::daemon::DataService service(configured_max_batch_delay());
    const char *const require_cuda = std::getenv("UGDR_REQUIRE_CUDA");
    if (service.initialization_status() != 0 && require_cuda != nullptr &&
        std::strcmp(require_cuda, "1") == 0) {
        notify_ready(ready_fd, 's');
        std::cerr << "ugdr_daemon: CUDA IPC backend unavailable: "
                  << service.initialization_status() << '\n';
        return 77;
    }
    ugdr::control::ControlIpcHandler handler(service);
    ugdr::ipc::IpcServer server(handler);
    const int start_status = server.start(socket_path);
    if (start_status != 0) {
        notify_ready(ready_fd, 'e');
        std::cerr << "ugdr_daemon: failed to start IPC server: " << -start_status << '\n';
        return 1;
    }
    notify_ready(ready_fd, 'r');

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    while (stop_requested == 0) {
        const int poll_status = server.poll_once(service.workers_ready() ? 0 : 10);
        if (poll_status != 0) {
            std::cerr << "ugdr_daemon: IPC poll failed: " << -poll_status << '\n';
            return 1;
        }
        service.progress_once();
        if (service.status() != 0) {
            std::cerr << "ugdr_daemon: data service failed: " << service.status() << '\n';
            return 1;
        }
    }
    service.request_stop();
    for (std::size_t iteration = 0; !service.stopped() && iteration < 1'000'000; ++iteration) {
        service.progress_once();
        if (service.status() != 0) {
            std::cerr << "ugdr_daemon: data service shutdown failed: " << service.status() << '\n';
            return 1;
        }
    }
    if (!service.stopped()) {
        std::cerr << "ugdr_daemon: data service shutdown timed out\n";
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::strcmp(argv[1], "--socket") == 0) {
        return run_server(argv[2]);
    }
    if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
        std::cout << "usage: ugdr_daemon [--socket PATH]\n";
        return 0;
    }
    if (argc != 1) {
        std::cerr << "usage: ugdr_daemon [--socket PATH]\n";
        return 2;
    }
    const char *const configured = std::getenv("UGDR_DAEMON_SOCKET");
    return run_server(configured != nullptr && configured[0] != '\0'
                          ? configured
                          : ugdr::control::kDefaultDaemonSocket);
}

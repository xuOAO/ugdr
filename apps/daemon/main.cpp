#include "control/control.hpp"
#include "gpu/gpu.hpp"
#include "ipc/ipc.hpp"
#include "worker/worker.hpp"

#include <csignal>
#include <cstring>
#include <iostream>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

extern "C" void request_stop(int) {
    stop_requested = 1;
}

int run_server(const char *socket_path) {
    ugdr::control::UnsupportedControlService service;
    ugdr::control::ControlIpcHandler handler(service);
    ugdr::ipc::IpcServer server(handler);
    const int start_status = server.start(socket_path);
    if (start_status != 0) {
        std::cerr << "ugdr_daemon: failed to start IPC server: " << -start_status << '\n';
        return 1;
    }

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    while (stop_requested == 0) {
        const int poll_status = server.poll_once(100);
        if (poll_status != 0) {
            std::cerr << "ugdr_daemon: IPC poll failed: " << -poll_status << '\n';
            return 1;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::strcmp(argv[1], "--socket") == 0) {
        return run_server(argv[2]);
    }
    if (argc != 1) {
        std::cerr << "usage: ugdr_daemon [--socket PATH]\n";
        return 2;
    }

    const int control_status = ugdr::control_placeholder();
    const int worker_status = ugdr::worker_placeholder();
    const int gpu_status = ugdr::gpu_placeholder();
    return control_status | worker_status | gpu_status;
}

#include "control/control.hpp"
#include "control/device_context.hpp"
#include "control/pd_mr_cq.hpp"
#include "gpu/cuda_ipc_memory.hpp"
#include "gpu/gpu.hpp"
#include "ipc/ipc.hpp"
#include "worker/worker.hpp"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

extern "C" void request_stop(int) {
    stop_requested = 1;
}

int run_server(const char *socket_path) {
    ugdr::gpu::RuntimeCudaIpcMemoryBackend memory_backend;
    ugdr::control::PdMrCqService service(memory_backend);
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

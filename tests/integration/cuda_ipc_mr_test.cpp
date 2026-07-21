#include "control/pd_mr_cq.hpp"
#include "gpu/cuda_ipc_memory.hpp"
#include "ugdr/api.hpp"

#include <cuda_runtime_api.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {

class FillingCudaBackend final : public ugdr::gpu::CudaIpcMemoryBackend {
  public:
    int initialization_status() const noexcept {
        return runtime.initialization_status();
    }

    int open(const ugdr::gpu::ExportedCudaMemory &memory,
             ugdr::gpu::CudaIpcMapping *mapping) override {
        const int open_status = runtime.open(memory, mapping);
        if (open_status != 0) {
            return open_status;
        }
        const int fill_status =
            runtime.fill(*mapping, memory.allocation_offset, memory.length, 0x5a);
        if (fill_status != 0) {
            (void)runtime.close(*mapping);
        }
        return fill_status;
    }

    int close(const ugdr::gpu::CudaIpcMapping &mapping) noexcept override {
        return runtime.close(mapping);
    }

    ugdr::gpu::RuntimeCudaIpcMemoryBackend runtime;
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

    FillingCudaBackend backend;
    ugdr::control::PdMrCqService service;
    int request_count = 0;
};

int child_main(const std::string &socket_path, int ready_fd) {
    CountingService service;
    if (service.backend.initialization_status() != 0) {
        const char skipped = 's';
        (void)::write(ready_fd, &skipped, 1);
        return 77;
    }
    ugdr::control::ControlIpcHandler handler(service);
    ugdr::ipc::IpcServer server(handler);
    if (server.start(socket_path) != 0) {
        return 20;
    }
    const char ready = 'r';
    if (::write(ready_fd, &ready, 1) != 1) {
        return 21;
    }
    for (int iteration = 0; iteration < 400 && service.request_count < 7; ++iteration) {
        if (server.poll_once(50) != 0) {
            return 22;
        }
    }
    return service.request_count == 7 && service.service.context_count() == 0 &&
                   service.service.pd_count() == 0 && service.service.mr_count() == 0
               ? 0
               : 23;
}

}  // namespace

int main() {
    char directory_template[] = "/tmp/ugdr-cuda-ipc-test-XXXXXX";
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
    if (::read(ready_pipe[0], &ready, 1) != 1) {
        return 4;
    }
    ::close(ready_pipe[0]);
    if (ready == 's') {
        int child_status = 0;
        (void)::waitpid(child, &child_status, 0);
        (void)::rmdir(directory);
        return 77;
    }
    if (ready != 'r' || ::setenv("UGDR_DAEMON_SOCKET", socket_path.c_str(), 1) != 0) {
        return 5;
    }

    constexpr std::size_t allocation_size = 4096;
    constexpr std::size_t offset = 128;
    constexpr std::size_t registered_length = 256;
    void *allocation = nullptr;
    if (cudaMalloc(&allocation, allocation_size) != cudaSuccess ||
        cudaMemset(allocation, 0, allocation_size) != cudaSuccess) {
        return 6;
    }

    int count = 0;
    ugdr_device **devices = ugdr_get_device_list(&count);
    ugdr_context *context =
        devices != nullptr && count == 1 ? ugdr_open_device(devices[0]) : nullptr;
    if (devices != nullptr) {
        ugdr_free_device_list(devices);
    }
    ugdr_pd *pd = context != nullptr ? ugdr_alloc_pd(context) : nullptr;
    auto *const registered_address = static_cast<unsigned char *>(allocation) + offset;
    int host_memory = 0;
    errno = 0;
    if (pd == nullptr ||
        ugdr_reg_mr(pd, &host_memory, sizeof(host_memory), UGDR_ACCESS_LOCAL_WRITE) != nullptr ||
        errno != EOPNOTSUPP) {
        return 7;
    }
    errno = 0;
    if (ugdr_reg_mr(pd, registered_address, registered_length, UGDR_ACCESS_REMOTE_WRITE) !=
            nullptr ||
        errno != EINVAL) {
        return 7;
    }
    errno = 0;
    if (ugdr_reg_mr(pd, static_cast<unsigned char *>(allocation) + allocation_size - 8, 16,
                    UGDR_ACCESS_LOCAL_WRITE) != nullptr ||
        errno != EINVAL) {
        return 7;
    }
    void *managed = nullptr;
    if (cudaMallocManaged(&managed, 64) != cudaSuccess) {
        return 7;
    }
    errno = 0;
    const bool managed_rejected =
        ugdr_reg_mr(pd, managed, 64, UGDR_ACCESS_LOCAL_WRITE) == nullptr && errno == EOPNOTSUPP;
    if (cudaFree(managed) != cudaSuccess || !managed_rejected) {
        return 7;
    }
    ugdr_mr *mr = pd != nullptr ? ugdr_reg_mr(pd, registered_address, registered_length,
                                              UGDR_ACCESS_LOCAL_WRITE | UGDR_ACCESS_REMOTE_WRITE)
                                : nullptr;
    if (mr == nullptr || mr->context != context || mr->pd != pd || mr->addr != registered_address ||
        mr->length != registered_length || mr->handle == 0 || mr->lkey == 0 || mr->rkey == 0) {
        return 7;
    }

    std::vector<unsigned char> observed(allocation_size);
    if (cudaMemcpy(observed.data(), allocation, observed.size(), cudaMemcpyDeviceToHost) !=
        cudaSuccess) {
        return 8;
    }
    for (std::size_t index = 0; index < observed.size(); ++index) {
        const unsigned char expected =
            index >= offset && index < offset + registered_length ? 0x5a : 0;
        if (observed[index] != expected) {
            return 9;
        }
    }

    if (ugdr_dereg_mr(mr) != 0 || ugdr_dealloc_pd(pd) != 0 || ugdr_close_device(context) != 0 ||
        cudaFree(allocation) != cudaSuccess) {
        return 10;
    }
    int child_status = 0;
    if (::waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) {
        return 11;
    }
    return ::rmdir(directory) == 0 ? 0 : 12;
}

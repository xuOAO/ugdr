#include "control/pd_mr_cq.hpp"
#include "queue/completion_queue.hpp"
#include "ugdr/api.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::atomic<std::size_t> allocation_count{};

class UnusedCudaBackend final : public ugdr::gpu::CudaIpcMemoryBackend {
  public:
    int open(const ugdr::gpu::ExportedCudaMemory &, ugdr::gpu::CudaIpcMapping *) override {
        return EIO;
    }
    int close(const ugdr::gpu::CudaIpcMapping &) noexcept override {
        return EIO;
    }
};

ugdr::queue::CompletionEntry completion(std::uint64_t id) {
    const bool receive = id % 2 == 0;
    return {id,
            UGDR_WC_SUCCESS,
            receive ? UGDR_WC_RECV_RDMA_WITH_IMM : UGDR_WC_RDMA_WRITE,
            static_cast<std::uint32_t>(id + 100),
            static_cast<std::uint32_t>(id + 200),
            77,
            receive ? UGDR_WC_WITH_IMM : 0U};
}

class CountingService final : public ugdr::control::ControlService {
  public:
    CountingService() : service(backend) {
    }

    ugdr::control::ControlServiceResult
    handle(ugdr::ipc::SessionId session_id, ugdr::control::DecodedControlRequest request) override {
        ++request_count;
        const auto method = static_cast<ugdr::control::ControlMethod>(request.value.method);
        auto result = service.handle(session_id, std::move(request));
        if (method == ugdr::control::ControlMethod::create_cq && result.response.status == 0) {
            capture_and_fill(result);
        }
        return result;
    }

    void on_disconnect(ugdr::ipc::SessionId session_id) noexcept override {
        service.on_disconnect(session_id);
    }

    UnusedCudaBackend backend;
    ugdr::control::PdMrCqService service;
    int request_count = 0;
    bool valid = true;

  private:
    void capture_and_fill(const ugdr::control::ControlServiceResult &result) {
        constexpr std::array capacities{128U, 4U, 4U};
        const std::size_t index = mappings.size();
        if (index >= capacities.size() || result.file_descriptors.size() != 1) {
            valid = false;
            return;
        }
        const int fd = ::fcntl(result.file_descriptors[0].get(), F_DUPFD_CLOEXEC, 0);
        ugdr::queue::SharedRing mapping;
        const ugdr::queue::QueueDescriptor descriptor{ugdr::queue::QueueKind::completion,
                                                      capacities[index],
                                                      ugdr::queue::completion_slot_stride()};
        if (fd < 0 || ugdr::queue::map_shared_ring(fd, descriptor, &mapping) != 0) {
            if (fd >= 0 && !mapping.valid()) {
                ::close(fd);
            }
            valid = false;
            return;
        }
        mappings.push_back(std::move(mapping));
        if (index == 0) {
            std::array<ugdr::queue::CompletionEntry, 96> entries{};
            for (std::size_t item = 0; item < entries.size(); ++item) {
                entries[item] = completion(item + 1);
            }
            valid = ugdr::queue::produce_completions(mappings.back(), entries.data(),
                                                     static_cast<int>(entries.size())) ==
                    static_cast<int>(entries.size());
        } else {
            const auto entry = completion(index == 1 ? 1001 : 2001);
            valid = ugdr::queue::produce_completions(mappings.back(), &entry, 1) == 1;
        }
    }

    std::vector<ugdr::queue::SharedRing> mappings;
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
    constexpr int expected_requests = 9;
    for (int iteration = 0; iteration < 500 && service.request_count < expected_requests;
         ++iteration) {
        if (server.poll_once(50) != 0) {
            return 22;
        }
    }
    return service.request_count == expected_requests && service.valid &&
                   service.service.context_count() == 0 && service.service.cq_count() == 0
               ? 0
               : 23;
}

bool valid_wc(const ugdr_wc &wc) {
    const auto expected = completion(wc.wr_id);
    return wc.status == static_cast<ugdr_wc_status>(expected.status) &&
           wc.opcode == static_cast<ugdr_wc_opcode>(expected.opcode) &&
           wc.byte_len == expected.byte_length && wc.imm_data == expected.immediate_data &&
           wc.qp_num == expected.qp_num && wc.wc_flags == expected.flags && wc.vendor_err == 0 &&
           wc.src_qp == 0 && wc.pkey_index == 0 && wc.slid == 0 && wc.sl == 0 &&
           wc.dlid_path_bits == 0;
}

bool poll_concurrently(ugdr_cq *cq) {
    std::array<std::atomic<int>, 97> seen{};
    seen[1] = 1;
    std::atomic<int> collected{1};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int thread_index = 0; thread_index < 8; ++thread_index) {
        threads.emplace_back([&] {
            while (collected.load(std::memory_order_acquire) < 96 && failures.load() == 0) {
                std::array<ugdr_wc, 8> completions{};
                const int count = ugdr_poll_cq(cq, completions.size(), completions.data());
                if (count < 0) {
                    ++failures;
                    return;
                }
                if (count == 0) {
                    std::this_thread::yield();
                    continue;
                }
                for (int index = 0; index < count; ++index) {
                    const auto id = completions[static_cast<std::size_t>(index)].wr_id;
                    if (id < 2 || id > 96 ||
                        !valid_wc(completions[static_cast<std::size_t>(index)]) ||
                        seen[static_cast<std::size_t>(id)].fetch_add(1) != 0) {
                        ++failures;
                    }
                }
                collected.fetch_add(count, std::memory_order_release);
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }
    if (failures.load() != 0 || collected.load() != 96) {
        return false;
    }
    for (std::size_t id = 1; id <= 96; ++id) {
        if (seen[id].load() != 1) {
            return false;
        }
    }
    return true;
}

}  // namespace

void *operator new(std::size_t size) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
    if (void *memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void *operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void *memory) noexcept {
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void *memory) noexcept {
    std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    char directory_template[] = "/tmp/ugdr-cq-polling-test-XXXXXX";
    char *const directory = ::mkdtemp(directory_template);
    if (directory == nullptr) {
        return 1;
    }
    const std::string socket_path = std::string(directory) + "/control.sock";
    std::array<int, 2> ready_pipe{};
    if (::pipe2(ready_pipe.data(), O_CLOEXEC) != 0) {
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
    if (::read(ready_pipe[0], &ready, 1) != 1 || ready != 'r' ||
        ::setenv("UGDR_DAEMON_SOCKET", socket_path.c_str(), 1) != 0) {
        return 4;
    }
    ::close(ready_pipe[0]);

    int count = 0;
    ugdr_device **devices = ugdr_get_device_list(&count);
    ugdr_context *const context =
        devices != nullptr && count == 1 ? ugdr_open_device(devices[0]) : nullptr;
    if (devices != nullptr) {
        ugdr_free_device_list(devices);
    }
    ugdr_cq *const shared =
        context != nullptr ? ugdr_create_cq(context, 128, nullptr, nullptr, 0) : nullptr;
    ugdr_cq *const send =
        context != nullptr ? ugdr_create_cq(context, 4, nullptr, nullptr, 0) : nullptr;
    ugdr_cq *const receive =
        context != nullptr ? ugdr_create_cq(context, 4, nullptr, nullptr, 0) : nullptr;
    if (context == nullptr || shared == nullptr || send == nullptr || receive == nullptr) {
        return 5;
    }

    ugdr_wc first{};
    if (ugdr_poll_cq(shared, 0, nullptr) != 0 || ugdr_poll_cq(shared, 1, &first) != 1 ||
        first.wr_id != 1 || !valid_wc(first) || !poll_concurrently(shared)) {
        return 6;
    }
    ugdr_wc sentinel{};
    sentinel.wr_id = 999;
    sentinel.vendor_err = 73;
    const ugdr_wc expected_sentinel = sentinel;
    if (ugdr_poll_cq(shared, 1, &sentinel) != 0 ||
        std::memcmp(&sentinel, &expected_sentinel, sizeof(sentinel)) != 0 ||
        ugdr_poll_cq(shared, -1, &sentinel) != -EINVAL ||
        std::memcmp(&sentinel, &expected_sentinel, sizeof(sentinel)) != 0) {
        return 7;
    }

    ugdr_wc send_wc{};
    ugdr_wc receive_wc{};
    const std::size_t allocations_before = allocation_count.load(std::memory_order_relaxed);
    const int send_count = ugdr_poll_cq(send, 1, &send_wc);
    const std::size_t allocations_after = allocation_count.load(std::memory_order_relaxed);
    if (send_count != 1 || allocations_before != allocations_after || send_wc.wr_id != 1001 ||
        !valid_wc(send_wc) || ugdr_poll_cq(receive, 1, &receive_wc) != 1 ||
        receive_wc.wr_id != 2001 || !valid_wc(receive_wc)) {
        return 8;
    }
    std::atomic<bool> polling_started{false};
    std::atomic<int> destroy_race_failure{0};
    std::thread poller([&] {
        polling_started.store(true, std::memory_order_release);
        for (;;) {
            ugdr_wc completion{};
            const int result = ugdr_poll_cq(shared, 1, &completion);
            if (result == -EINVAL) {
                return;
            }
            if (result != 0) {
                destroy_race_failure = 1;
                return;
            }
            std::this_thread::yield();
        }
    });
    while (!polling_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    const int destroy_status = ugdr_destroy_cq(shared);
    poller.join();
    if (destroy_status != 0 || destroy_race_failure.load() != 0 ||
        ugdr_poll_cq(shared, 1, &sentinel) != -EINVAL ||
        std::memcmp(&sentinel, &expected_sentinel, sizeof(sentinel)) != 0 ||
        ugdr_destroy_cq(send) != 0 || ugdr_destroy_cq(receive) != 0 ||
        ugdr_close_device(context) != 0) {
        return 9;
    }

    int child_status = 0;
    if (::waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) {
        return 10;
    }
    return ::rmdir(directory) == 0 ? 0 : 11;
}

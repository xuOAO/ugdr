#include "control/qp.hpp"
#include "queue/descriptors.hpp"
#include "queue/shared_ring.hpp"
#include "ugdr/api.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
        const auto method = static_cast<ugdr::control::ControlMethod>(request.value.method);
        auto result = service.handle(session_id, std::move(request));
        if (method == ugdr::control::ControlMethod::create_qp && result.response.status == 0) {
            capture_qp(result);
        }
        return result;
    }

    void on_disconnect(ugdr::ipc::SessionId session_id) noexcept override {
        service.on_disconnect(session_id);
    }

    void drain() {
        for (Observer &observer : observers) {
            drain_ring(observer.send, &observer.send_wr_ids, true);
            drain_ring(observer.receive, &observer.receive_wr_ids, false);
        }
    }

    bool posting_is_valid() const {
        if (!valid || observers.size() != 2 || observers[0].send_wr_ids.size() != 17 ||
            observers[1].send_wr_ids.size() != 16 ||
            observers[0].receive_wr_ids != std::vector<std::uint64_t>{43} ||
            !observers[1].receive_wr_ids.empty() ||
            std::count(observers[0].send_wr_ids.begin(), observers[0].send_wr_ids.end(), 41) != 1) {
            return false;
        }
        for (int index = 0; index < 16; ++index) {
            const std::uint64_t first = UINT64_C(1000) + static_cast<std::uint64_t>(index) * 2;
            const auto &ids = observers[static_cast<std::size_t>(index % 2)].send_wr_ids;
            const auto found = std::find(ids.begin(), ids.end(), first);
            if (found == ids.end() || std::next(found) == ids.end() ||
                *std::next(found) != first + 1 || std::count(ids.begin(), ids.end(), first) != 1 ||
                std::count(ids.begin(), ids.end(), first + 1) != 1) {
                return false;
            }
        }
        return true;
    }

    UnusedCudaBackend backend;
    ugdr::control::QpService service;
    int request_count = 0;

  private:
    struct Observer {
        ugdr::queue::SharedRing send;
        ugdr::queue::SharedRing receive;
        std::vector<std::uint64_t> send_wr_ids;
        std::vector<std::uint64_t> receive_wr_ids;
    };

    void capture_qp(const ugdr::control::ControlServiceResult &result) {
        if (result.file_descriptors.size() != 2) {
            valid = false;
            return;
        }
        std::uint32_t send_stride = 0;
        std::uint32_t receive_stride = 0;
        if (ugdr::queue::send_slot_stride(2, &send_stride) != 0 ||
            ugdr::queue::receive_slot_stride(2, &receive_stride) != 0) {
            valid = false;
            return;
        }
        Observer observer;
        const ugdr::queue::QueueDescriptor send_descriptor{ugdr::queue::QueueKind::send, 128,
                                                           send_stride};
        const ugdr::queue::QueueDescriptor receive_descriptor{ugdr::queue::QueueKind::receive, 32,
                                                              receive_stride};
        const int send_fd = ::fcntl(result.file_descriptors[0].get(), F_DUPFD_CLOEXEC, 0);
        const int receive_fd = ::fcntl(result.file_descriptors[1].get(), F_DUPFD_CLOEXEC, 0);
        if (send_fd < 0 || receive_fd < 0 ||
            ugdr::queue::map_shared_ring(send_fd, send_descriptor, &observer.send) != 0 ||
            ugdr::queue::map_shared_ring(receive_fd, receive_descriptor, &observer.receive) != 0) {
            if (send_fd >= 0 && !observer.send.valid()) {
                ::close(send_fd);
            }
            if (receive_fd >= 0 && !observer.receive.valid()) {
                ::close(receive_fd);
            }
            valid = false;
            return;
        }
        observers.push_back(std::move(observer));
    }

    static void drain_ring(ugdr::queue::SharedRing &ring, std::vector<std::uint64_t> *wr_ids,
                           bool send) {
        ugdr::queue::ConstSlotBatch batch;
        while (ring.consumer_peek(ring.descriptor().capacity, &batch) == 0) {
            auto consume_span = [&](ugdr::queue::ConstSlotSpan span) {
                const auto *slot = static_cast<const std::byte *>(span.data);
                for (std::uint32_t index = 0; index < span.count; ++index) {
                    const auto *current =
                        slot + static_cast<std::size_t>(index) * ring.descriptor().slot_stride;
                    wr_ids->push_back(
                        send ? reinterpret_cast<const ugdr::queue::SendWqeHeader *>(current)->wr_id
                             : reinterpret_cast<const ugdr::queue::ReceiveWqeHeader *>(current)
                                   ->wr_id);
                }
            };
            consume_span(batch.first);
            consume_span(batch.second);
            if (ring.consumer_release(batch.count) != 0) {
                return;
            }
        }
    }

    std::vector<Observer> observers;
    bool valid = true;
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
    constexpr int expected_control_requests = 19;
    for (int iteration = 0; iteration < 500 && service.request_count < expected_control_requests;
         ++iteration) {
        service.drain();
        if (server.poll_once(50) != 0) {
            return 22;
        }
    }
    service.drain();
    return service.request_count == expected_control_requests && service.service.qp_count() == 0 &&
                   service.service.cq_count() == 0 && service.service.pd_count() == 0 &&
                   service.service.context_count() == 0 && service.posting_is_valid()
               ? 0
               : 23;
}

ugdr_qp_init_attr qp_attributes(ugdr_cq *send_cq, ugdr_cq *recv_cq) {
    return {send_cq, recv_cq, 128, 32, 2, 2, UGDR_QPT_RC, 0};
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
    retry.timeout = 17;
    retry.retry_cnt = 3;
    retry.rnr_retry = 7;
    retry.min_rnr_timer = 19;
    constexpr int mask =
        UGDR_QP_TIMEOUT | UGDR_QP_RETRY_CNT | UGDR_QP_RNR_RETRY | UGDR_QP_MIN_RNR_TIMER;
    return ugdr_connect_qp(qp, &remote, &retry, mask);
}

bool post_concurrently(ugdr_qp *first, ugdr_qp *second) {
    std::atomic<int> failures{};
    std::vector<std::thread> threads;
    for (int index = 0; index < 16; ++index) {
        threads.emplace_back([&, index] {
            ugdr_send_wr second_wr{};
            second_wr.wr_id = UINT64_C(1001) + static_cast<std::uint64_t>(index) * 2;
            second_wr.opcode = UGDR_WR_RDMA_WRITE_WITH_IMM;
            second_wr.imm_data = static_cast<std::uint32_t>(index);
            second_wr.wr.rdma.remote_addr =
                UINT64_C(0x100040) + static_cast<std::uint64_t>(index) * 128;
            second_wr.wr.rdma.rkey = 31;
            ugdr_send_wr first_wr{};
            first_wr.wr_id = UINT64_C(1000) + static_cast<std::uint64_t>(index) * 2;
            first_wr.next = &second_wr;
            first_wr.opcode = UGDR_WR_RDMA_WRITE;
            first_wr.send_flags = index % 3 == 0 ? UGDR_SEND_SIGNALED : 0;
            first_wr.wr.rdma.remote_addr =
                UINT64_C(0x100000) + static_cast<std::uint64_t>(index) * 128;
            first_wr.wr.rdma.rkey = 31;
            ugdr_send_wr *bad = nullptr;
            ugdr_qp *const qp = index % 2 == 0 ? first : second;
            if (ugdr_post_send(qp, &first_wr, &bad) != 0 || bad != nullptr) {
                ++failures;
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }
    return failures.load() == 0;
}

}  // namespace

int main() {
    char directory_template[] = "/tmp/ugdr-qp-posting-test-XXXXXX";
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
    ugdr_pd *const pd = context != nullptr ? ugdr_alloc_pd(context) : nullptr;
    ugdr_cq *const send_cq =
        context != nullptr ? ugdr_create_cq(context, 128, nullptr, nullptr, 0) : nullptr;
    ugdr_cq *const recv_cq =
        context != nullptr ? ugdr_create_cq(context, 128, nullptr, nullptr, 0) : nullptr;
    ugdr_qp_init_attr first_attributes = qp_attributes(send_cq, recv_cq);
    ugdr_qp_init_attr second_attributes = qp_attributes(send_cq, recv_cq);
    ugdr_qp *const first = pd != nullptr ? ugdr_create_qp(pd, &first_attributes) : nullptr;
    ugdr_qp *const second = pd != nullptr ? ugdr_create_qp(pd, &second_attributes) : nullptr;
    if (context == nullptr || pd == nullptr || send_cq == nullptr || recv_cq == nullptr ||
        first == nullptr || second == nullptr) {
        return 5;
    }

    ugdr_send_wr send{};
    send.wr_id = 41;
    send.opcode = UGDR_WR_RDMA_WRITE;
    ugdr_send_wr *bad_send = nullptr;
    ugdr_recv_wr receive{43, nullptr, nullptr, 0};
    ugdr_recv_wr *bad_receive = nullptr;
    if (ugdr_post_send(first, &send, &bad_send) != EINVAL || bad_send != &send ||
        ugdr_post_recv(first, &receive, &bad_receive) != EINVAL || bad_receive != &receive ||
        initialize(first) != 0 || initialize(second) != 0) {
        return 6;
    }

    bad_send = nullptr;
    bad_receive = nullptr;
    if (ugdr_post_send(first, &send, &bad_send) != EINVAL || bad_send != &send ||
        ugdr_post_recv(first, &receive, &bad_receive) != 0 || bad_receive != nullptr) {
        return 7;
    }

    ugdr_qp_conn_info first_info{};
    ugdr_qp_conn_info second_info{};
    if (ugdr_query_qp_conn_info(first, &first_info) != 0 ||
        ugdr_query_qp_conn_info(second, &second_info) != 0 || first_info.qp_num == 0 ||
        second_info.qp_num == 0 || connect(first, second_info.qp_num) != 0 ||
        connect(second, first_info.qp_num) != 0 || !post_concurrently(first, second)) {
        return 8;
    }

    const ugdr_send_wr expected_send = send;
    bad_send = reinterpret_cast<ugdr_send_wr *>(static_cast<std::uintptr_t>(1));
    if (ugdr_post_send(first, &send, &bad_send) != 0 ||
        bad_send != reinterpret_cast<ugdr_send_wr *>(static_cast<std::uintptr_t>(1)) ||
        std::memcmp(&send, &expected_send, sizeof(send)) != 0) {
        return 9;
    }
    if (ugdr_destroy_qp(first) != 0 || ugdr_destroy_qp(second) != 0 ||
        ugdr_destroy_cq(send_cq) != 0 || ugdr_destroy_cq(recv_cq) != 0 ||
        ugdr_dealloc_pd(pd) != 0 || ugdr_close_device(context) != 0) {
        return 10;
    }

    int child_status = 0;
    if (::waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) {
        return 11;
    }
    return ::rmdir(directory) == 0 ? 0 : 12;
}

#include "control/qp.hpp"
#include "control/queue_descriptor.hpp"
#include "support/mock_worker_fixture.hpp"
#include "ugdr/api.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
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

class ProgressingService final : public ugdr::control::ControlService {
  public:
    ProgressingService() : service(backend), tracker(false), worker(&tracker) {
        completion_queues.reserve(3);
        queue_pairs.reserve(4);
    }

    ugdr::control::ControlServiceResult
    handle(ugdr::ipc::SessionId session_id, ugdr::control::DecodedControlRequest request) override {
        ++request_count;
        const auto method = static_cast<ugdr::control::ControlMethod>(request.value.method);
        const std::uint64_t request_identity = request.value.object_identity;
        ugdr::control::QpCreateAttributes create_attributes;
        bool create_attributes_valid = false;
        std::uint32_t requested_state = UGDR_QPS_UNKNOWN;
        if (method == ugdr::control::ControlMethod::create_qp) {
            create_attributes_valid = ugdr::control::decode_qp_create_attributes(
                                          request.value.opaque, &create_attributes) == 0;
        } else if (method == ugdr::control::ControlMethod::modify_qp &&
                   request.value.opaque.size() >= 12) {
            std::uint32_t encoded = 0;
            std::memcpy(&encoded, request.value.opaque.data() + 8, sizeof(encoded));
            requested_state = ntohl(encoded);
        }

        auto result = service.handle(session_id, std::move(request));
        if (result.response.status != 0) {
            return result;
        }
        switch (method) {
        case ugdr::control::ControlMethod::create_cq:
            capture_cq(result);
            break;
        case ugdr::control::ControlMethod::create_qp:
            if (!create_attributes_valid) {
                valid = false;
            } else {
                capture_qp(result, create_attributes);
            }
            break;
        case ugdr::control::ControlMethod::modify_qp:
            if (requested_state == UGDR_QPS_ERR) {
                if (auto *qp = find_qp(request_identity); qp != nullptr) {
                    qp->mock.err = true;
                } else {
                    valid = false;
                }
            }
            break;
        case ugdr::control::ControlMethod::destroy_qp:
            if (auto *qp = find_qp(request_identity); qp != nullptr) {
                qp->live = false;
            } else {
                valid = false;
            }
            break;
        default:
            break;
        }
        return result;
    }

    void on_disconnect(ugdr::ipc::SessionId session_id) noexcept override {
        service.on_disconnect(session_id);
    }

    bool progress_all() {
        bool any = false;
        bool round_progress = false;
        do {
            round_progress = false;
            for (QpObserver &observer : queue_pairs) {
                if (observer.live && worker.progress_once(observer.mock)) {
                    round_progress = true;
                    any = true;
                }
            }
        } while (round_progress);
        return any;
    }

    [[nodiscard]] bool finished_cleanly() const {
        return valid && tracker.balanced() && service.qp_count() == 0 && service.cq_count() == 0 &&
               service.pd_count() == 0 && service.context_count() == 0;
    }

    UnusedCudaBackend backend;
    ugdr::control::QpService service;
    ugdr::test::LifecycleTracker tracker;
    ugdr::test::MockWorker worker;
    int request_count = 0;
    bool valid = true;

  private:
    struct CqObserver {
        std::uint64_t identity = 0;
        ugdr::queue::SharedRing ring;
    };

    struct QpObserver {
        ugdr::test::MockQp mock;
        std::uint64_t identity = 0;
        bool live = true;
    };

    CqObserver *find_cq(std::uint64_t identity) {
        for (CqObserver &observer : completion_queues) {
            if (observer.identity == identity) {
                return &observer;
            }
        }
        return nullptr;
    }

    QpObserver *find_qp(std::uint64_t identity) {
        for (QpObserver &observer : queue_pairs) {
            if (observer.identity == identity) {
                return &observer;
            }
        }
        return nullptr;
    }

    void capture_cq(const ugdr::control::ControlServiceResult &result) {
        std::vector<ugdr::queue::QueueDescriptor> descriptors;
        if (result.file_descriptors.size() != 1 ||
            ugdr::control::decode_queue_descriptors(result.response.opaque, &descriptors) != 0 ||
            descriptors.size() != 1) {
            valid = false;
            return;
        }
        const int fd = ::fcntl(result.file_descriptors[0].get(), F_DUPFD_CLOEXEC, 0);
        CqObserver observer;
        observer.identity = result.response.object_identity;
        if (fd < 0 || ugdr::queue::map_shared_ring(fd, descriptors[0], &observer.ring) != 0) {
            if (fd >= 0 && !observer.ring.valid()) {
                ::close(fd);
            }
            valid = false;
            return;
        }
        completion_queues.push_back(std::move(observer));
    }

    void capture_qp(const ugdr::control::ControlServiceResult &result,
                    const ugdr::control::QpCreateAttributes &attributes) {
        std::vector<ugdr::queue::QueueDescriptor> descriptors;
        CqObserver *const send_cq = find_cq(attributes.send_cq_identity);
        CqObserver *const recv_cq = find_cq(attributes.recv_cq_identity);
        if (send_cq == nullptr || recv_cq == nullptr || result.file_descriptors.size() != 2 ||
            ugdr::control::decode_queue_descriptors(result.response.opaque, &descriptors) != 0 ||
            descriptors.size() != 2) {
            valid = false;
            return;
        }
        const int send_fd = ::fcntl(result.file_descriptors[0].get(), F_DUPFD_CLOEXEC, 0);
        const int receive_fd = ::fcntl(result.file_descriptors[1].get(), F_DUPFD_CLOEXEC, 0);
        QpObserver observer;
        observer.identity = result.response.object_identity;
        observer.mock.qp_num = static_cast<std::uint32_t>(queue_pairs.size() + 1);
        observer.mock.sq_sig_all = attributes.sq_sig_all != 0;
        observer.mock.send_cq = &send_cq->ring;
        observer.mock.recv_cq = &recv_cq->ring;
        if (send_fd < 0 || receive_fd < 0 ||
            ugdr::queue::map_shared_ring(send_fd, descriptors[0], &observer.mock.sq) != 0 ||
            ugdr::queue::map_shared_ring(receive_fd, descriptors[1], &observer.mock.rq) != 0) {
            if (send_fd >= 0 && !observer.mock.sq.valid()) {
                ::close(send_fd);
            }
            if (receive_fd >= 0 && !observer.mock.rq.valid()) {
                ::close(receive_fd);
            }
            valid = false;
            return;
        }
        queue_pairs.push_back(std::move(observer));
        if (queue_pairs.size() % 2 == 0) {
            QpObserver &first = queue_pairs[queue_pairs.size() - 2];
            QpObserver &second = queue_pairs.back();
            first.mock.peer = &second.mock;
            second.mock.peer = &first.mock;
        }
    }

    std::vector<CqObserver> completion_queues;
    std::vector<QpObserver> queue_pairs;
};

int child_main(const std::string &socket_path, int ready_fd) {
    ProgressingService service;
    ugdr::control::ControlIpcHandler handler(service);
    ugdr::ipc::IpcServer server(handler);
    const int start_status = server.start(socket_path);
    if (start_status != 0) {
        std::cerr << "queue API server start failed: " << start_status << " errno=" << errno
                  << '\n';
        return 20;
    }
    const char ready = 'r';
    if (::write(ready_fd, &ready, 1) != 1) {
        return 21;
    }
    constexpr int expected_control_requests = 32;
    for (int iteration = 0; iteration < 4000 && service.request_count < expected_control_requests;
         ++iteration) {
        (void)service.progress_all();
        if (server.poll_once(5) != 0) {
            return 22;
        }
        (void)service.progress_all();
    }
    return service.request_count == expected_control_requests && service.finished_cleanly() ? 0
                                                                                            : 23;
}

ugdr_qp_init_attr qp_attributes(ugdr_cq *send_cq, ugdr_cq *recv_cq, int sq_sig_all) {
    return {send_cq, recv_cq, 32, 32, 4, 4, UGDR_QPT_RC, sq_sig_all};
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

bool post_receive(ugdr_qp *qp, std::uint64_t wr_id) {
    ugdr_recv_wr receive{};
    receive.wr_id = wr_id;
    ugdr_recv_wr *bad = nullptr;
    return ugdr_post_recv(qp, &receive, &bad) == 0 && bad == nullptr;
}

bool post_send(ugdr_qp *qp, std::uint64_t wr_id, ugdr_wr_opcode opcode, unsigned int flags,
               std::uint32_t immediate, int num_sge = 1) {
    std::array<ugdr_sge, 4> sges{};
    for (int index = 0; index < num_sge; ++index) {
        sges[static_cast<std::size_t>(index)] = {
            UINT64_C(0x100000) + static_cast<std::uint64_t>(index) * 64,
            static_cast<std::uint32_t>(8 + index), static_cast<std::uint32_t>(31 + index)};
    }
    ugdr_send_wr send{};
    send.wr_id = wr_id;
    send.sg_list = sges.data();
    send.num_sge = num_sge;
    send.opcode = opcode;
    send.send_flags = flags;
    send.imm_data = immediate;
    send.wr.rdma.remote_addr = UINT64_C(0x200000);
    send.wr.rdma.rkey = 77;
    ugdr_send_wr *bad = nullptr;
    return ugdr_post_send(qp, &send, &bad) == 0 && bad == nullptr;
}

int poll_until(ugdr_cq *cq, ugdr_wc *entries, int wanted) {
    int collected = 0;
    for (int iteration = 0; iteration < 5000 && collected < wanted; ++iteration) {
        const int count = ugdr_poll_cq(cq, wanted - collected, entries + collected);
        if (count < 0) {
            return count;
        }
        collected += count;
        if (count == 0) {
            ::usleep(1000);
        }
    }
    return collected;
}

bool connect_pair(ugdr_qp *first, ugdr_qp *second, std::uint32_t *first_num,
                  std::uint32_t *second_num) {
    ugdr_qp_conn_info first_info{};
    ugdr_qp_conn_info second_info{};
    if (initialize(first) != 0 || initialize(second) != 0 ||
        ugdr_query_qp_conn_info(first, &first_info) != 0 ||
        ugdr_query_qp_conn_info(second, &second_info) != 0 || first_info.qp_num == 0 ||
        second_info.qp_num == 0 || connect(first, second_info.qp_num) != 0 ||
        connect(second, first_info.qp_num) != 0) {
        return false;
    }
    *first_num = first_info.qp_num;
    *second_num = second_info.qp_num;
    return true;
}

bool common_cq_semantics(ugdr_qp *first, ugdr_qp *second, ugdr_cq *common, std::uint32_t first_num,
                         std::uint32_t second_num) {
    constexpr std::uint32_t immediate = UINT32_C(0x44332211);
    if (!post_receive(second, 200) ||
        !post_send(first, 100, UGDR_WR_RDMA_WRITE_WITH_IMM, UGDR_SEND_SIGNALED, immediate, 4)) {
        return false;
    }
    std::array<ugdr_wc, 2> completions{};
    const int first_count = poll_until(common, completions.data(), completions.size());
    if (first_count != 2 || completions[0].wr_id != 100 ||
        completions[0].status != UGDR_WC_SUCCESS || completions[0].opcode != UGDR_WC_RDMA_WRITE ||
        completions[0].qp_num != first_num || completions[1].wr_id != 200 ||
        completions[1].status != UGDR_WC_SUCCESS ||
        completions[1].opcode != UGDR_WC_RECV_RDMA_WITH_IMM || completions[1].byte_len != 38 ||
        completions[1].imm_data != immediate || completions[1].qp_num != second_num ||
        completions[1].wc_flags != UGDR_WC_WITH_IMM) {
        std::cerr << "common CQ first matrix failed: count=" << first_count
                  << " first=" << completions[0].wr_id << "/" << completions[0].qp_num
                  << " second=" << completions[1].wr_id << "/" << completions[1].qp_num
                  << " byte_len=" << completions[1].byte_len << '\n';
        return false;
    }

    if (!post_send(first, 101, UGDR_WR_RDMA_WRITE, 0, 0) ||
        !post_send(first, 102, UGDR_WR_RDMA_WRITE, UGDR_SEND_SIGNALED, 0) ||
        !post_send(second, 103, UGDR_WR_RDMA_WRITE, 0, 0)) {
        return false;
    }
    completions = {};
    const int second_count = poll_until(common, completions.data(), completions.size());
    const bool valid =
        second_count == 2 && ((completions[0].wr_id == 102 && completions[1].wr_id == 103) ||
                              (completions[0].wr_id == 103 && completions[1].wr_id == 102));
    if (!valid) {
        std::cerr << "common CQ signaling failed: count=" << second_count
                  << " first=" << completions[0].wr_id << " second=" << completions[1].wr_id
                  << '\n';
    }
    return valid;
}

bool backpressure_and_allocation(ugdr_qp *first, ugdr_qp *second, ugdr_cq *send_cq,
                                 ugdr_cq *recv_cq) {
    if (!post_receive(second, 300) || !post_receive(second, 301) ||
        !post_send(first, 400, UGDR_WR_RDMA_WRITE_WITH_IMM, UGDR_SEND_SIGNALED, 7) ||
        !post_send(first, 401, UGDR_WR_RDMA_WRITE_WITH_IMM, UGDR_SEND_SIGNALED, 9)) {
        return false;
    }
    ugdr_wc first_send{};
    ugdr_wc first_receive{};
    if (poll_until(send_cq, &first_send, 1) != 1 || poll_until(recv_cq, &first_receive, 1) != 1 ||
        first_send.wr_id != 400 || first_receive.wr_id != 300) {
        return false;
    }
    ugdr_wc second_send{};
    ugdr_wc second_receive{};
    if (poll_until(send_cq, &second_send, 1) != 1 || poll_until(recv_cq, &second_receive, 1) != 1 ||
        second_send.wr_id != 401 || second_receive.wr_id != 301) {
        return false;
    }

    const std::size_t before = allocation_count.load(std::memory_order_relaxed);
    if (!post_send(first, 402, UGDR_WR_RDMA_WRITE, UGDR_SEND_SIGNALED, 0)) {
        return false;
    }
    ugdr_wc completion{};
    const int count = poll_until(send_cq, &completion, 1);
    const std::size_t after = allocation_count.load(std::memory_order_relaxed);
    return count == 1 && completion.wr_id == 402 && before == after;
}

bool concurrent_post_and_poll(ugdr_qp *qp, ugdr_cq *cq) {
    constexpr int kCount = 16;
    std::atomic<int> failures{};
    std::vector<std::thread> posters;
    posters.reserve(kCount);
    for (int index = 0; index < kCount; ++index) {
        posters.emplace_back([&, index] {
            if (!post_send(qp, UINT64_C(1000) + static_cast<std::uint64_t>(index),
                           UGDR_WR_RDMA_WRITE, UGDR_SEND_SIGNALED, 0)) {
                ++failures;
            }
        });
    }
    for (auto &thread : posters) {
        thread.join();
    }
    if (failures.load() != 0) {
        return false;
    }

    std::array<std::atomic<int>, kCount> seen{};
    std::atomic<int> collected{};
    std::vector<std::thread> pollers;
    for (int thread_index = 0; thread_index < 4; ++thread_index) {
        pollers.emplace_back([&] {
            while (collected.load(std::memory_order_acquire) < kCount &&
                   failures.load(std::memory_order_relaxed) == 0) {
                std::array<ugdr_wc, 4> entries{};
                const int count = ugdr_poll_cq(cq, entries.size(), entries.data());
                if (count < 0) {
                    ++failures;
                    return;
                }
                if (count == 0) {
                    std::this_thread::yield();
                    continue;
                }
                for (int index = 0; index < count; ++index) {
                    const std::uint64_t wr_id = entries[static_cast<std::size_t>(index)].wr_id;
                    if (wr_id < 1000 || wr_id >= 1000 + kCount ||
                        seen[static_cast<std::size_t>(wr_id - 1000)].fetch_add(1) != 0) {
                        ++failures;
                    }
                }
                collected.fetch_add(count, std::memory_order_release);
            }
        });
    }
    for (auto &thread : pollers) {
        thread.join();
    }
    if (failures.load() != 0 || collected.load() != kCount) {
        return false;
    }
    for (const auto &count : seen) {
        if (count.load() != 1) {
            return false;
        }
    }
    return true;
}

bool flush_semantics(ugdr_qp *qp, ugdr_cq *common) {
    if (!post_send(qp, 900, UGDR_WR_RDMA_WRITE, 0, 0) || !post_receive(qp, 901)) {
        return false;
    }
    ugdr_qp_attr attributes{};
    attributes.qp_state = UGDR_QPS_ERR;
    if (ugdr_modify_qp(qp, &attributes, UGDR_QP_STATE) != 0) {
        return false;
    }
    std::array<ugdr_wc, 2> completions{};
    return poll_until(common, completions.data(), completions.size()) == 2 &&
           completions[0].wr_id == 900 && completions[1].wr_id == 901 &&
           completions[0].status == UGDR_WC_WR_FLUSH_ERR &&
           completions[1].status == UGDR_WC_WR_FLUSH_ERR;
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
    char directory_template[] = "/tmp/ugdr-queue-api-test-XXXXXX";
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
    const ssize_t ready_result = ::read(ready_pipe[0], &ready, 1);
    if (ready_result != 1 || ready != 'r') {
        int child_status = 0;
        (void)::waitpid(child, &child_status, 0);
        std::cerr << "queue API child failed before ready: read=" << ready_result
                  << " status=" << child_status << '\n';
        return 4;
    }
    if (::setenv("UGDR_DAEMON_SOCKET", socket_path.c_str(), 1) != 0) {
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
    ugdr_cq *const common =
        context != nullptr ? ugdr_create_cq(context, 32, nullptr, nullptr, 0) : nullptr;
    ugdr_cq *const send =
        context != nullptr ? ugdr_create_cq(context, 32, nullptr, nullptr, 0) : nullptr;
    ugdr_cq *const receive =
        context != nullptr ? ugdr_create_cq(context, 1, nullptr, nullptr, 0) : nullptr;

    ugdr_qp_init_attr common_first_attr = qp_attributes(common, common, 0);
    ugdr_qp_init_attr common_second_attr = qp_attributes(common, common, 1);
    ugdr_qp_init_attr separate_first_attr = qp_attributes(send, receive, 0);
    ugdr_qp_init_attr separate_second_attr = qp_attributes(send, receive, 0);
    ugdr_qp *const common_first = pd != nullptr ? ugdr_create_qp(pd, &common_first_attr) : nullptr;
    ugdr_qp *const common_second =
        pd != nullptr ? ugdr_create_qp(pd, &common_second_attr) : nullptr;
    ugdr_qp *const separate_first =
        pd != nullptr ? ugdr_create_qp(pd, &separate_first_attr) : nullptr;
    ugdr_qp *const separate_second =
        pd != nullptr ? ugdr_create_qp(pd, &separate_second_attr) : nullptr;
    if (context == nullptr || pd == nullptr || common == nullptr || send == nullptr ||
        receive == nullptr || common_first == nullptr || common_second == nullptr ||
        separate_first == nullptr || separate_second == nullptr) {
        return 5;
    }

    std::uint32_t common_first_num = 0;
    std::uint32_t common_second_num = 0;
    std::uint32_t separate_first_num = 0;
    std::uint32_t separate_second_num = 0;
    if (!connect_pair(common_first, common_second, &common_first_num, &common_second_num) ||
        !connect_pair(separate_first, separate_second, &separate_first_num, &separate_second_num)) {
        return 6;
    }

    ugdr_send_wr invalid{};
    invalid.opcode = static_cast<ugdr_wr_opcode>(99);
    ugdr_send_wr *bad = nullptr;
    if (ugdr_post_send(common_first, &invalid, &bad) != EINVAL || bad != &invalid) {
        return 70;
    }
    if (!common_cq_semantics(common_first, common_second, common, common_first_num,
                             common_second_num)) {
        return 71;
    }
    if (!backpressure_and_allocation(separate_first, separate_second, send, receive)) {
        return 72;
    }
    if (!concurrent_post_and_poll(separate_first, send)) {
        return 73;
    }
    if (!flush_semantics(common_second, common)) {
        return 74;
    }

    if (ugdr_destroy_qp(common_first) != 0 || ugdr_destroy_qp(common_second) != 0 ||
        ugdr_destroy_qp(separate_first) != 0 || ugdr_destroy_qp(separate_second) != 0 ||
        ugdr_destroy_cq(common) != 0 || ugdr_destroy_cq(send) != 0 ||
        ugdr_destroy_cq(receive) != 0 || ugdr_dealloc_pd(pd) != 0 ||
        ugdr_close_device(context) != 0) {
        return 8;
    }

    int child_status = 0;
    if (::waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) {
        return 9;
    }
    return ::rmdir(directory) == 0 ? 0 : 10;
}

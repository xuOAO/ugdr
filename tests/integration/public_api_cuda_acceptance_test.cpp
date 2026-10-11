#include "ugdr/api.hpp"

#include <cuda_runtime_api.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kAllocationBytes = 8192;
constexpr std::chrono::seconds kCompletionTimeout{30};

enum class Role : std::uint32_t {
    requester,
    responder,
};

enum class CommandCode : std::uint32_t {
    connect,
    post_receive,
    send,
    verify_target,
    check_send_cq_empty,
    check_receive_cq_empty,
    deregister_target,
    reregister_target,
    shutdown,
};

struct Command {
    CommandCode code{};
    std::uint32_t peer_qp_num = 0;
    std::uint32_t rkey = 0;
    std::uint32_t length = 0;
    std::uint32_t source_offset = 0;
    std::uint32_t target_offset = 0;
    std::uint32_t immediate_data = 0;
    std::uint32_t send_flags = 0;
    std::uint32_t opcode = UGDR_WR_RDMA_WRITE;
    std::uint32_t expect_receive = 0;
    std::uint32_t apply_copy = 0;
    std::uint64_t remote_address = 0;
    std::uint64_t wr_id = 0;
    std::uint64_t receive_wr_id = 0;
};

struct Report {
    int status = 0;
    int completion_count = 0;
    std::uint32_t qp_num = 0;
    std::uint32_t rkey = 0;
    std::uint64_t address = 0;
    ugdr_wc completion{};
};

struct ClientPipes {
    std::array<int, 2> commands{-1, -1};
    std::array<int, 2> reports{-1, -1};
};

bool write_all(int fd, const void *data, std::size_t size) {
    const auto *cursor = static_cast<const unsigned char *>(data);
    while (size != 0) {
        const ssize_t written = ::write(fd, cursor, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        cursor += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

bool read_all(int fd, void *data, std::size_t size) {
    auto *cursor = static_cast<unsigned char *>(data);
    while (size != 0) {
        const ssize_t received = ::read(fd, cursor, size);
        if (received == 0) {
            return false;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        cursor += received;
        size -= static_cast<std::size_t>(received);
    }
    return true;
}

bool send_command(int fd, const Command &command) {
    return write_all(fd, &command, sizeof(command));
}

bool send_report(int fd, const Report &report) {
    return write_all(fd, &report, sizeof(report));
}

unsigned char source_byte(std::size_t index) {
    return static_cast<unsigned char>((index * 17 + 29) & 0xffU);
}

int initialize_qp(ugdr_qp *qp) {
    ugdr_qp_attr attributes{};
    attributes.qp_state = UGDR_QPS_INIT;
    attributes.cur_qp_state = UGDR_QPS_RESET;
    attributes.qp_access_flags = UGDR_ACCESS_REMOTE_WRITE;
    return ugdr_modify_qp(qp, &attributes,
                          UGDR_QP_STATE | UGDR_QP_CUR_STATE | UGDR_QP_ACCESS_FLAGS);
}

int connect_qp(ugdr_qp *qp, std::uint32_t remote_qp_num) {
    const ugdr_qp_conn_info remote{remote_qp_num};
    ugdr_qp_attr retry{};
    retry.timeout = 1;
    retry.retry_cnt = 1;
    retry.rnr_retry = 1;
    retry.min_rnr_timer = 1;
    constexpr int mask =
        UGDR_QP_TIMEOUT | UGDR_QP_RETRY_CNT | UGDR_QP_RNR_RETRY | UGDR_QP_MIN_RNR_TIMER;
    return ugdr_connect_qp(qp, &remote, &retry, mask);
}

int poll_until(ugdr_cq *cq, ugdr_wc *completion) {
    const auto deadline = Clock::now() + kCompletionTimeout;
    while (Clock::now() < deadline) {
        const int count = ugdr_poll_cq(cq, 1, completion);
        if (count != 0) {
            return count;
        }
        (void)::poll(nullptr, 0, 1);
    }
    return 0;
}

int requester_command(const Command &command, ugdr_qp *qp, ugdr_cq *send_cq, void *source,
                      ugdr_mr *source_mr, Report *report) {
    if (command.code == CommandCode::check_send_cq_empty) {
        report->completion = {};
        report->completion_count = ugdr_poll_cq(send_cq, 1, &report->completion);
        return report->completion_count == 0 ? 0 : EPROTO;
    }
    if (command.code != CommandCode::send || command.source_offset > kAllocationBytes ||
        command.length > kAllocationBytes - command.source_offset) {
        return EINVAL;
    }
    ugdr_sge sge{reinterpret_cast<std::uint64_t>(source) + command.source_offset, command.length,
                 source_mr->lkey};
    ugdr_send_wr wr{};
    wr.wr_id = command.wr_id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = static_cast<ugdr_wr_opcode>(command.opcode);
    wr.send_flags = command.send_flags;
    wr.imm_data = command.immediate_data;
    wr.wr.rdma.remote_addr = command.remote_address;
    wr.wr.rdma.rkey = command.rkey;
    ugdr_send_wr *bad = nullptr;
    const int post_status = ugdr_post_send(qp, &wr, &bad);
    if (post_status != 0 || bad != nullptr) {
        return post_status != 0 ? post_status : EPROTO;
    }
    if ((command.send_flags & UGDR_SEND_SIGNALED) == 0) {
        report->completion_count = 0;
        return 0;
    }
    report->completion_count = poll_until(send_cq, &report->completion);
    return report->completion_count == 1 ? 0 : EPROTO;
}

int responder_command(const Command &command, ugdr_qp *qp, ugdr_cq *receive_cq, void *target,
                      ugdr_pd *pd, ugdr_mr **target_mr, std::vector<unsigned char> *expected,
                      Report *report) {
    if (command.code == CommandCode::post_receive) {
        ugdr_recv_wr receive{};
        receive.wr_id = command.receive_wr_id;
        ugdr_recv_wr *bad = nullptr;
        const int status = ugdr_post_recv(qp, &receive, &bad);
        return status == 0 && bad == nullptr ? 0 : (status != 0 ? status : EPROTO);
    }
    if (command.code == CommandCode::check_receive_cq_empty) {
        report->completion = {};
        report->completion_count = ugdr_poll_cq(receive_cq, 1, &report->completion);
        return report->completion_count == 0 ? 0 : EPROTO;
    }
    if (command.code == CommandCode::deregister_target) {
        if (*target_mr == nullptr) {
            return EINVAL;
        }
        const int status = ugdr_dereg_mr(*target_mr);
        if (status == 0) {
            *target_mr = nullptr;
        }
        return status;
    }
    if (command.code == CommandCode::reregister_target) {
        if (*target_mr != nullptr) {
            return EINVAL;
        }
        *target_mr = ugdr_reg_mr(pd, target, kAllocationBytes,
                                 UGDR_ACCESS_LOCAL_WRITE | UGDR_ACCESS_REMOTE_WRITE);
        if (*target_mr == nullptr) {
            return errno == 0 ? EIO : errno;
        }
        report->rkey = (*target_mr)->rkey;
        report->address = reinterpret_cast<std::uint64_t>(target);
        return 0;
    }
    if (command.code != CommandCode::verify_target || command.target_offset > kAllocationBytes ||
        command.length > kAllocationBytes - command.target_offset ||
        command.source_offset > kAllocationBytes ||
        command.length > kAllocationBytes - command.source_offset) {
        return EINVAL;
    }
    if (command.apply_copy != 0) {
        for (std::size_t index = 0; index < command.length; ++index) {
            (*expected)[command.target_offset + index] = source_byte(command.source_offset + index);
        }
    }
    std::vector<unsigned char> observed(kAllocationBytes);
    const auto deadline = Clock::now() + kCompletionTimeout;
    bool matched = false;
    do {
        if (cudaMemcpy(observed.data(), target, observed.size(), cudaMemcpyDeviceToHost) !=
            cudaSuccess) {
            return EIO;
        }
        matched = observed == *expected;
        if (!matched) {
            (void)::poll(nullptr, 0, 1);
        }
    } while (!matched && Clock::now() < deadline);
    if (!matched) {
        return EPROTO;
    }
    if (command.expect_receive != 0) {
        report->completion_count = poll_until(receive_cq, &report->completion);
        if (report->completion_count != 1 || report->completion.wr_id != command.receive_wr_id ||
            report->completion.status != UGDR_WC_SUCCESS ||
            report->completion.opcode != UGDR_WC_RECV_RDMA_WITH_IMM ||
            report->completion.byte_len != command.length ||
            report->completion.imm_data != command.immediate_data ||
            report->completion.wc_flags != UGDR_WC_WITH_IMM) {
            return EPROTO;
        }
    }
    return 0;
}

int client_main(Role role, const std::string &socket_path, int command_fd, int report_fd) {
    if (::setenv("UGDR_DAEMON_SOCKET", socket_path.c_str(), 1) != 0) {
        return 20;
    }
    void *buffer = nullptr;
    if (cudaMalloc(&buffer, kAllocationBytes) != cudaSuccess) {
        (void)send_report(report_fd, Report{77});
        return 77;
    }
    std::vector<unsigned char> host(kAllocationBytes, role == Role::requester ? 0 : UINT8_C(0xa5));
    if (role == Role::requester) {
        for (std::size_t index = 0; index < host.size(); ++index) {
            host[index] = source_byte(index);
        }
    }
    if (cudaMemcpy(buffer, host.data(), host.size(), cudaMemcpyHostToDevice) != cudaSuccess ||
        cudaDeviceSynchronize() != cudaSuccess) {
        (void)cudaFree(buffer);
        (void)send_report(report_fd, Report{77});
        return 77;
    }

    int device_count = 0;
    ugdr_device **devices = ugdr_get_device_list(&device_count);
    ugdr_context *context =
        devices != nullptr && device_count == 1 ? ugdr_open_device(devices[0]) : nullptr;
    if (devices != nullptr) {
        ugdr_free_device_list(devices);
    }
    ugdr_pd *pd = context == nullptr ? nullptr : ugdr_alloc_pd(context);
    ugdr_mr *mr = pd == nullptr
                      ? nullptr
                      : ugdr_reg_mr(pd, buffer, kAllocationBytes,
                                    role == Role::requester
                                        ? UGDR_ACCESS_LOCAL_WRITE
                                        : UGDR_ACCESS_LOCAL_WRITE | UGDR_ACCESS_REMOTE_WRITE);
    ugdr_cq *send_cq =
        context == nullptr ? nullptr : ugdr_create_cq(context, 32, nullptr, nullptr, 0);
    ugdr_cq *receive_cq =
        role == Role::requester
            ? send_cq
            : (context == nullptr ? nullptr : ugdr_create_cq(context, 32, nullptr, nullptr, 0));
    ugdr_qp_init_attr creation{send_cq, receive_cq, 32, 32, 1, 1, UGDR_QPT_RC, 0};
    ugdr_qp *qp = pd == nullptr || send_cq == nullptr || receive_cq == nullptr
                      ? nullptr
                      : ugdr_create_qp(pd, &creation);
    ugdr_qp_conn_info info{};
    int setup_status = context == nullptr || pd == nullptr || mr == nullptr || send_cq == nullptr ||
                               receive_cq == nullptr || qp == nullptr
                           ? EIO
                           : initialize_qp(qp);
    if (setup_status == 0) {
        setup_status = ugdr_query_qp_conn_info(qp, &info);
    }
    if (setup_status == 0 && role == Role::requester) {
        ugdr_sge sge{reinterpret_cast<std::uint64_t>(buffer), 1, mr->lkey};
        ugdr_send_wr wr{};
        wr.wr_id = 1;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = UGDR_WR_RDMA_WRITE;
        ugdr_send_wr *bad = nullptr;
        if (ugdr_post_send(qp, &wr, &bad) != EINVAL || bad != &wr) {
            setup_status = EPROTO;
        }
    }
    Report setup_report;
    setup_report.status = setup_status;
    setup_report.qp_num = info.qp_num;
    if (role == Role::responder && mr != nullptr) {
        setup_report.address = reinterpret_cast<std::uint64_t>(buffer);
        setup_report.rkey = mr->rkey;
    }
    if (!send_report(report_fd, setup_report) || setup_status != 0) {
        return 21;
    }

    for (;;) {
        Command command{};
        if (!read_all(command_fd, &command, sizeof(command))) {
            return 22;
        }
        Report report;
        if (command.code == CommandCode::connect) {
            report.status = connect_qp(qp, command.peer_qp_num);
        } else if (command.code == CommandCode::shutdown) {
            int cleanup_status = 0;
            if (qp != nullptr && ugdr_destroy_qp(qp) != 0) {
                cleanup_status = EPROTO;
            }
            qp = nullptr;
            if (receive_cq != nullptr && receive_cq != send_cq &&
                ugdr_destroy_cq(receive_cq) != 0) {
                cleanup_status = EPROTO;
            }
            receive_cq = nullptr;
            if (send_cq != nullptr && ugdr_destroy_cq(send_cq) != 0) {
                cleanup_status = EPROTO;
            }
            send_cq = nullptr;
            if (mr != nullptr && ugdr_dereg_mr(mr) != 0) {
                cleanup_status = EPROTO;
            }
            mr = nullptr;
            if (pd != nullptr && ugdr_dealloc_pd(pd) != 0) {
                cleanup_status = EPROTO;
            }
            pd = nullptr;
            if (context != nullptr && ugdr_close_device(context) != 0) {
                cleanup_status = EPROTO;
            }
            context = nullptr;
            if (cudaFree(buffer) != cudaSuccess) {
                cleanup_status = EPROTO;
            }
            report.status = cleanup_status;
            (void)send_report(report_fd, report);
            return cleanup_status == 0 ? 0 : 23;
        } else if (role == Role::requester) {
            report.status = requester_command(command, qp, send_cq, buffer, mr, &report);
        } else {
            report.status =
                responder_command(command, qp, receive_cq, buffer, pd, &mr, &host, &report);
        }
        report.qp_num = info.qp_num;
        if (!send_report(report_fd, report)) {
            return 24;
        }
    }
}

void close_pipes(ClientPipes *pipes) {
    for (int &fd : pipes->commands) {
        if (fd >= 0) {
            (void)::close(fd);
            fd = -1;
        }
    }
    for (int &fd : pipes->reports) {
        if (fd >= 0) {
            (void)::close(fd);
            fd = -1;
        }
    }
}

void terminate_process(pid_t *process) {
    if (*process <= 0) {
        return;
    }
    (void)::kill(*process, SIGTERM);
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
        const pid_t waited = ::waitpid(*process, nullptr, WNOHANG);
        if (waited == *process || (waited < 0 && errno != EINTR)) {
            *process = -1;
            return;
        }
        (void)::poll(nullptr, 0, 1);
    }
    (void)::kill(*process, SIGKILL);
    while (::waitpid(*process, nullptr, 0) < 0 && errno == EINTR) {
    }
    *process = -1;
}

bool transact(ClientPipes &pipes, const Command &command, Report *report) {
    return send_command(pipes.commands[1], command) &&
           read_all(pipes.reports[0], report, sizeof(*report));
}

bool successful_send(const Report &report, const Command &command, std::uint32_t qp_num,
                     std::uint32_t expected_status) {
    return report.status == 0 && report.completion_count == 1 &&
           report.completion.wr_id == command.wr_id &&
           report.completion.status == expected_status &&
           report.completion.opcode == UGDR_WC_RDMA_WRITE && report.completion.qp_num == qp_num;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        return 1;
    }
    char directory_template[] = "/tmp/ugdr-public-api-cuda-XXXXXX";
    char *const directory = ::mkdtemp(directory_template);
    if (directory == nullptr) {
        return 2;
    }
    const std::string socket_path = std::string(directory) + "/control.sock";
    std::array<int, 2> ready_pipe{};
    if (::pipe2(ready_pipe.data(), O_CLOEXEC) != 0) {
        (void)::rmdir(directory);
        return 3;
    }
    pid_t daemon = ::fork();
    if (daemon < 0) {
        (void)::rmdir(directory);
        return 4;
    }
    if (daemon == 0) {
        (void)::close(ready_pipe[0]);
        const std::string ready_fd = std::to_string(ready_pipe[1]);
        if (::fcntl(ready_pipe[1], F_SETFD, 0) != 0 ||
            ::setenv("UGDR_DAEMON_READY_FD", ready_fd.c_str(), 1) != 0 ||
            ::setenv("UGDR_REQUIRE_CUDA", "1", 1) != 0) {
            std::_Exit(126);
        }
        ::execl(argv[1], argv[1], "--socket", socket_path.c_str(), nullptr);
        std::_Exit(127);
    }
    (void)::close(ready_pipe[1]);

    ClientPipes requester;
    ClientPipes responder;
    pid_t requester_process = -1;
    pid_t responder_process = -1;
    const auto finish = [&](int result) {
        if (result != 0 && result != 77) {
            std::fprintf(stderr, "public API CUDA acceptance failed at stage %d\n", result);
        }
        close_pipes(&requester);
        close_pipes(&responder);
        terminate_process(&requester_process);
        terminate_process(&responder_process);
        terminate_process(&daemon);
        (void)::unlink(socket_path.c_str());
        (void)::rmdir(directory);
        return result;
    };

    char ready = 0;
    if (::read(ready_pipe[0], &ready, 1) != 1) {
        (void)::close(ready_pipe[0]);
        return finish(5);
    }
    (void)::close(ready_pipe[0]);
    if (ready == 's') {
        int daemon_status = 0;
        (void)::waitpid(daemon, &daemon_status, 0);
        daemon = -1;
        return finish(77);
    }
    if (ready != 'r' || ::pipe2(requester.commands.data(), O_CLOEXEC) != 0 ||
        ::pipe2(requester.reports.data(), O_CLOEXEC) != 0 ||
        ::pipe2(responder.commands.data(), O_CLOEXEC) != 0 ||
        ::pipe2(responder.reports.data(), O_CLOEXEC) != 0) {
        return finish(6);
    }

    requester_process = ::fork();
    if (requester_process < 0) {
        return finish(7);
    }
    if (requester_process == 0) {
        (void)::close(requester.commands[1]);
        (void)::close(requester.reports[0]);
        close_pipes(&responder);
        std::_Exit(
            client_main(Role::requester, socket_path, requester.commands[0], requester.reports[1]));
    }
    responder_process = ::fork();
    if (responder_process < 0) {
        return finish(8);
    }
    if (responder_process == 0) {
        (void)::close(responder.commands[1]);
        (void)::close(responder.reports[0]);
        close_pipes(&requester);
        std::_Exit(
            client_main(Role::responder, socket_path, responder.commands[0], responder.reports[1]));
    }
    (void)::close(requester.commands[0]);
    requester.commands[0] = -1;
    (void)::close(requester.reports[1]);
    requester.reports[1] = -1;
    (void)::close(responder.commands[0]);
    responder.commands[0] = -1;
    (void)::close(responder.reports[1]);
    responder.reports[1] = -1;

    Report requester_report;
    Report responder_report;
    if (!read_all(requester.reports[0], &requester_report, sizeof(requester_report)) ||
        !read_all(responder.reports[0], &responder_report, sizeof(responder_report))) {
        return finish(9);
    }
    if (requester_report.status == 77 || responder_report.status == 77) {
        return finish(77);
    }
    if (requester_report.status != 0 || responder_report.status != 0 ||
        requester_report.qp_num == 0 || responder_report.qp_num == 0 ||
        responder_report.address == 0 || responder_report.rkey == 0) {
        return finish(10);
    }
    const std::uint64_t target_address = responder_report.address;
    const std::uint32_t target_rkey = responder_report.rkey;

    Command command;
    command.code = CommandCode::connect;
    command.peer_qp_num = responder_report.qp_num;
    if (!transact(requester, command, &requester_report) || requester_report.status != 0) {
        return finish(11);
    }
    command.peer_qp_num = requester_report.qp_num;
    if (!transact(responder, command, &responder_report) || responder_report.status != 0) {
        return finish(12);
    }
    const std::uint32_t requester_qp_num = requester_report.qp_num;

    command = {};
    command.code = CommandCode::post_receive;
    command.receive_wr_id = 900;
    if (!transact(responder, command, &responder_report) || responder_report.status != 0) {
        return finish(13);
    }

    command = {};
    command.code = CommandCode::send;
    command.rkey = target_rkey;
    command.length = 256;
    command.source_offset = 0;
    command.target_offset = 128;
    command.remote_address = target_address + command.target_offset;
    command.wr_id = 100;
    command.send_flags = UGDR_SEND_SIGNALED;
    if (!transact(requester, command, &requester_report) ||
        !successful_send(requester_report, command, requester_qp_num, UGDR_WC_SUCCESS)) {
        return finish(14);
    }
    Command verify = command;
    verify.code = CommandCode::verify_target;
    verify.apply_copy = 1;
    if (!transact(responder, verify, &responder_report) || responder_report.status != 0) {
        return finish(15);
    }
    Command empty{};
    empty.code = CommandCode::check_receive_cq_empty;
    if (!transact(responder, empty, &responder_report) || responder_report.status != 0) {
        return finish(16);
    }

    command = {};
    command.code = CommandCode::send;
    command.rkey = target_rkey;
    command.length = 128;
    command.source_offset = 512;
    command.target_offset = 512;
    command.remote_address = target_address + command.target_offset;
    command.wr_id = 101;
    if (!transact(requester, command, &requester_report) || requester_report.status != 0 ||
        requester_report.completion_count != 0) {
        return finish(17);
    }
    verify = command;
    verify.code = CommandCode::verify_target;
    verify.apply_copy = 1;
    if (!transact(responder, verify, &responder_report) || responder_report.status != 0) {
        return finish(18);
    }
    empty = {};
    empty.code = CommandCode::check_send_cq_empty;
    if (!transact(requester, empty, &requester_report) || requester_report.status != 0) {
        return finish(19);
    }

    command = {};
    command.code = CommandCode::send;
    command.rkey = target_rkey;
    command.length = 192;
    command.source_offset = 1024;
    command.target_offset = 1024;
    command.remote_address = target_address + command.target_offset;
    command.wr_id = 102;
    command.send_flags = UGDR_SEND_SIGNALED;
    command.opcode = UGDR_WR_RDMA_WRITE_WITH_IMM;
    command.immediate_data = UINT32_C(0x12345678);
    if (!transact(requester, command, &requester_report) ||
        !successful_send(requester_report, command, requester_qp_num, UGDR_WC_SUCCESS)) {
        return finish(20);
    }
    verify = command;
    verify.code = CommandCode::verify_target;
    verify.apply_copy = 1;
    verify.expect_receive = 1;
    verify.receive_wr_id = 900;
    if (!transact(responder, verify, &responder_report) || responder_report.status != 0 ||
        responder_report.completion.qp_num != responder_report.qp_num) {
        return finish(21);
    }

    command.target_offset = 1400;
    command.source_offset = 1400;
    command.length = 64;
    command.remote_address = target_address + command.target_offset;
    command.wr_id = 103;
    if (!transact(requester, command, &requester_report) ||
        !successful_send(requester_report, command, requester_qp_num, UGDR_WC_RNR_RETRY_EXC_ERR)) {
        return finish(22);
    }
    verify = command;
    verify.code = CommandCode::verify_target;
    if (!transact(responder, verify, &responder_report) || responder_report.status != 0) {
        return finish(23);
    }

    command.opcode = UGDR_WR_RDMA_WRITE;
    command.immediate_data = 0;
    command.target_offset = 1500;
    command.source_offset = 1500;
    command.remote_address = target_address + command.target_offset;
    command.rkey = target_rkey + 1;
    command.wr_id = 104;
    if (!transact(requester, command, &requester_report) ||
        !successful_send(requester_report, command, requester_qp_num, UGDR_WC_REM_ACCESS_ERR)) {
        return finish(24);
    }
    verify = command;
    verify.code = CommandCode::verify_target;
    if (!transact(responder, verify, &responder_report) || responder_report.status != 0) {
        return finish(25);
    }

    command.rkey = target_rkey;
    command.source_offset = 1600;
    command.target_offset = kAllocationBytes - 32;
    command.length = 64;
    command.remote_address = target_address + command.target_offset;
    command.wr_id = 105;
    if (!transact(requester, command, &requester_report) ||
        !successful_send(requester_report, command, requester_qp_num, UGDR_WC_REM_ACCESS_ERR)) {
        return finish(26);
    }
    verify = {};
    verify.code = CommandCode::verify_target;
    if (!transact(responder, verify, &responder_report) || responder_report.status != 0) {
        return finish(27);
    }

    Command mr_command{};
    mr_command.code = CommandCode::deregister_target;
    if (!transact(responder, mr_command, &responder_report) || responder_report.status != 0) {
        return finish(28);
    }
    command.rkey = target_rkey;
    command.target_offset = 1700;
    command.source_offset = 1700;
    command.length = 64;
    command.remote_address = target_address + command.target_offset;
    command.wr_id = 106;
    if (!transact(requester, command, &requester_report) ||
        !successful_send(requester_report, command, requester_qp_num, UGDR_WC_REM_ACCESS_ERR)) {
        return finish(29);
    }
    verify = command;
    verify.code = CommandCode::verify_target;
    if (!transact(responder, verify, &responder_report) || responder_report.status != 0) {
        return finish(30);
    }
    mr_command.code = CommandCode::reregister_target;
    if (!transact(responder, mr_command, &responder_report) || responder_report.status != 0 ||
        responder_report.rkey == 0 || responder_report.rkey == target_rkey) {
        return finish(31);
    }

    Command shutdown{};
    shutdown.code = CommandCode::shutdown;
    if (!transact(requester, shutdown, &requester_report) || requester_report.status != 0) {
        return finish(32);
    }
    int child_status = 0;
    if (::waitpid(requester_process, &child_status, 0) != requester_process ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        return finish(33);
    }
    requester_process = -1;
    if (!transact(responder, shutdown, &responder_report) || responder_report.status != 0) {
        return finish(34);
    }
    if (::waitpid(responder_process, &child_status, 0) != responder_process ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        return finish(35);
    }
    responder_process = -1;

    if (::kill(daemon, SIGTERM) != 0 || ::waitpid(daemon, &child_status, 0) != daemon ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        return finish(36);
    }
    daemon = -1;
    return finish(0);
}

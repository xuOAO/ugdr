#include "ugdr/api.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

enum class CommandCode : std::uint32_t {
    connect,
    query_state,
    disconnect,
    verify_peer_disconnect,
};

struct Command {
    CommandCode code{};
    std::uint32_t qp_num = 0;
};

struct Report {
    int status = 0;
    std::uint32_t qp_num = 0;
    ugdr_qp_state state = UGDR_QPS_UNKNOWN;
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

bool send_command(int fd, CommandCode code, std::uint32_t qp_num = 0) {
    const Command command{code, qp_num};
    return write_all(fd, &command, sizeof(command));
}

bool send_report(int fd, int status, std::uint32_t qp_num = 0,
                 ugdr_qp_state state = UGDR_QPS_UNKNOWN) {
    const Report report{status, qp_num, state};
    return write_all(fd, &report, sizeof(report));
}

ugdr_qp_init_attr qp_init_attributes(ugdr_cq *send_cq, ugdr_cq *recv_cq) {
    return {send_cq, recv_cq, 32, 32, 1, 1, UGDR_QPT_RC, 0};
}

int initialize_qp(ugdr_qp *qp) {
    ugdr_qp_attr attributes{};
    attributes.qp_state = UGDR_QPS_INIT;
    attributes.cur_qp_state = UGDR_QPS_RESET;
    attributes.qp_access_flags = UGDR_ACCESS_REMOTE_WRITE;
    return ugdr_modify_qp(qp, &attributes,
                          UGDR_QP_STATE | UGDR_QP_CUR_STATE | UGDR_QP_ACCESS_FLAGS);
}

int query_state(ugdr_qp *qp, ugdr_qp_state *state) {
    ugdr_qp_attr attributes{};
    ugdr_qp_init_attr creation{};
    const int status = ugdr_query_qp(qp, &attributes, UGDR_QP_STATE, &creation);
    if (status == 0) {
        *state = attributes.qp_state;
    }
    return status;
}

int connect_qp(ugdr_qp *qp, std::uint32_t remote_qp_num) {
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

bool invalid_query_preserves_outputs(ugdr_qp *qp) {
    ugdr_qp_attr attributes{};
    attributes.qp_state = UGDR_QPS_UNKNOWN;
    attributes.retry_cnt = 5;
    ugdr_qp_init_attr creation{};
    creation.max_send_wr = 77;
    const ugdr_qp_attr expected_attributes = attributes;
    const ugdr_qp_init_attr expected_creation = creation;
    return ugdr_query_qp(qp, &attributes, 1U << 30U, &creation) == EINVAL &&
           std::memcmp(&attributes, &expected_attributes, sizeof(attributes)) == 0 &&
           std::memcmp(&creation, &expected_creation, sizeof(creation)) == 0;
}

bool posting_and_poll_contract(ugdr_qp *qp, ugdr_cq *cq) {
    ugdr_wc completion{};
    completion.wr_id = 91;
    completion.vendor_err = 17;
    const ugdr_wc expected_completion = completion;

    ugdr_send_wr send{};
    send.wr_id = 93;
    send.opcode = UGDR_WR_RDMA_WRITE;
    const ugdr_send_wr expected_send = send;
    auto *bad_send = reinterpret_cast<ugdr_send_wr *>(static_cast<std::uintptr_t>(1));

    ugdr_recv_wr recv{};
    recv.wr_id = 95;
    const ugdr_recv_wr expected_recv = recv;
    auto *bad_recv = reinterpret_cast<ugdr_recv_wr *>(static_cast<std::uintptr_t>(2));

    return ugdr_poll_cq(cq, 1, &completion) == -EOPNOTSUPP &&
           std::memcmp(&completion, &expected_completion, sizeof(completion)) == 0 &&
           ugdr_post_send(qp, &send, &bad_send) == 0 &&
           bad_send == reinterpret_cast<ugdr_send_wr *>(static_cast<std::uintptr_t>(1)) &&
           std::memcmp(&send, &expected_send, sizeof(send)) == 0 &&
           ugdr_post_recv(qp, &recv, &bad_recv) == 0 &&
           bad_recv == reinterpret_cast<ugdr_recv_wr *>(static_cast<std::uintptr_t>(2)) &&
           std::memcmp(&recv, &expected_recv, sizeof(recv)) == 0;
}

int client_main(const std::string &socket_path, int command_fd, int report_fd) {
    if (::setenv("UGDR_DAEMON_SOCKET", socket_path.c_str(), 1) != 0) {
        return 20;
    }
    int device_count = 0;
    ugdr_device **devices = ugdr_get_device_list(&device_count);
    if (devices == nullptr || device_count != 1 || devices[0] == nullptr) {
        return 21;
    }
    ugdr_context *const context = ugdr_open_device(devices[0]);
    ugdr_free_device_list(devices);
    ugdr_pd *const pd = context != nullptr ? ugdr_alloc_pd(context) : nullptr;
    ugdr_cq *const send_cq =
        context != nullptr ? ugdr_create_cq(context, 64, nullptr, nullptr, 0) : nullptr;
    ugdr_cq *const recv_cq =
        context != nullptr ? ugdr_create_cq(context, 64, nullptr, nullptr, 0) : nullptr;
    ugdr_qp_init_attr creation = qp_init_attributes(send_cq, recv_cq);
    ugdr_qp *const qp = pd != nullptr && send_cq != nullptr && recv_cq != nullptr
                            ? ugdr_create_qp(pd, &creation)
                            : nullptr;
    ugdr_qp_conn_info info{};
    if (context == nullptr || pd == nullptr || send_cq == nullptr || recv_cq == nullptr ||
        qp == nullptr || initialize_qp(qp) != 0 || ugdr_query_qp_conn_info(qp, &info) != 0 ||
        info.qp_num == 0 || !invalid_query_preserves_outputs(qp) ||
        !send_report(report_fd, 0, info.qp_num, UGDR_QPS_INIT)) {
        return 22;
    }

    for (;;) {
        Command command{};
        if (!read_all(command_fd, &command, sizeof(command))) {
            return 23;
        }
        if (command.code == CommandCode::disconnect) {
            return 0;
        }
        if (command.code == CommandCode::query_state) {
            ugdr_qp_state state = UGDR_QPS_UNKNOWN;
            const int status = query_state(qp, &state);
            if (!send_report(report_fd, status, info.qp_num, state)) {
                return 24;
            }
            continue;
        }
        if (command.code == CommandCode::connect) {
            const int status = connect_qp(qp, command.qp_num);
            ugdr_qp_state state = UGDR_QPS_UNKNOWN;
            const int query_status = query_state(qp, &state);
            const int report_status = status != 0 ? status : query_status;
            if (!send_report(report_fd, report_status, info.qp_num, state)) {
                return 25;
            }
            continue;
        }
        if (command.code != CommandCode::verify_peer_disconnect) {
            return 26;
        }

        ugdr_qp_state live_state = UGDR_QPS_UNKNOWN;
        ugdr_qp_init_attr new_creation = qp_init_attributes(send_cq, recv_cq);
        ugdr_qp *const new_qp = ugdr_create_qp(pd, &new_creation);
        ugdr_qp_conn_info new_info{};
        int status = query_state(qp, &live_state);
        if (status == 0 && live_state != UGDR_QPS_RTS) {
            status = EPROTO;
        }
        if (status == 0 && (new_qp == nullptr || initialize_qp(new_qp) != 0 ||
                            ugdr_query_qp_conn_info(new_qp, &new_info) != 0 ||
                            new_info.qp_num == 0 || new_info.qp_num == command.qp_num)) {
            status = EPROTO;
        }
        if (status == 0 && connect_qp(new_qp, command.qp_num) != ENOENT) {
            status = EPROTO;
        }
        ugdr_qp_state failed_connect_state = UGDR_QPS_UNKNOWN;
        if (status == 0 &&
            (query_state(new_qp, &failed_connect_state) != 0 ||
             failed_connect_state != UGDR_QPS_INIT || !posting_and_poll_contract(qp, send_cq))) {
            status = EPROTO;
        }
        if (new_qp != nullptr && ugdr_destroy_qp(new_qp) != 0 && status == 0) {
            status = EPROTO;
        }
        if ((ugdr_destroy_qp(qp) != 0 || ugdr_destroy_cq(send_cq) != 0 ||
             ugdr_destroy_cq(recv_cq) != 0 || ugdr_dealloc_pd(pd) != 0 ||
             ugdr_close_device(context) != 0) &&
            status == 0) {
            status = EPROTO;
        }
        return send_report(report_fd, status, new_info.qp_num, live_state) ? status : 27;
    }
}

void close_pipe(ClientPipes *pipes) {
    for (int &fd : pipes->commands) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
    for (int &fd : pipes->reports) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
}

void terminate_process(pid_t *process) {
    if (*process <= 0) {
        return;
    }
    (void)::kill(*process, SIGTERM);
    while (::waitpid(*process, nullptr, 0) < 0 && errno == EINTR) {
    }
    *process = -1;
}

bool wait_for_socket(const std::string &socket_path, pid_t *daemon) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        struct stat status {};
        if (::lstat(socket_path.c_str(), &status) == 0 && S_ISSOCK(status.st_mode)) {
            return true;
        }
        int child_status = 0;
        if (::waitpid(*daemon, &child_status, WNOHANG) == *daemon) {
            *daemon = -1;
            return false;
        }
        (void)::poll(nullptr, 0, 20);
    }
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        return 1;
    }
    char directory_template[] = "/tmp/ugdr-control-acceptance-XXXXXX";
    char *const directory = ::mkdtemp(directory_template);
    if (directory == nullptr) {
        return 2;
    }
    const std::string socket_path = std::string(directory) + "/control.sock";
    pid_t daemon = ::fork();
    if (daemon < 0) {
        ::rmdir(directory);
        return 3;
    }
    if (daemon == 0) {
        ::execl(argv[1], argv[1], "--socket", socket_path.c_str(), nullptr);
        std::_Exit(127);
    }

    ClientPipes a;
    ClientPipes b;
    pid_t client_a = -1;
    pid_t client_b = -1;
    const auto finish = [&](int result) {
        close_pipe(&a);
        close_pipe(&b);
        terminate_process(&client_a);
        terminate_process(&client_b);
        terminate_process(&daemon);
        (void)::unlink(socket_path.c_str());
        (void)::rmdir(directory);
        return result;
    };
    if (!wait_for_socket(socket_path, &daemon)) {
        return finish(4);
    }
    if (::pipe2(a.commands.data(), O_CLOEXEC) != 0 || ::pipe2(a.reports.data(), O_CLOEXEC) != 0 ||
        ::pipe2(b.commands.data(), O_CLOEXEC) != 0 || ::pipe2(b.reports.data(), O_CLOEXEC) != 0) {
        return finish(5);
    }

    client_a = ::fork();
    if (client_a < 0) {
        return finish(6);
    }
    if (client_a == 0) {
        ::close(a.commands[1]);
        ::close(a.reports[0]);
        close_pipe(&b);
        const int result = client_main(socket_path, a.commands[0], a.reports[1]);
        std::_Exit(result);
    }
    client_b = ::fork();
    if (client_b < 0) {
        return finish(7);
    }
    if (client_b == 0) {
        ::close(b.commands[1]);
        ::close(b.reports[0]);
        close_pipe(&a);
        const int result = client_main(socket_path, b.commands[0], b.reports[1]);
        std::_Exit(result);
    }
    ::close(a.commands[0]);
    a.commands[0] = -1;
    ::close(a.reports[1]);
    a.reports[1] = -1;
    ::close(b.commands[0]);
    b.commands[0] = -1;
    ::close(b.reports[1]);
    b.reports[1] = -1;

    Report a_report{};
    Report b_report{};
    if (!read_all(a.reports[0], &a_report, sizeof(a_report)) || a_report.status != 0 ||
        a_report.qp_num == 0 || a_report.state != UGDR_QPS_INIT ||
        !read_all(b.reports[0], &b_report, sizeof(b_report)) || b_report.status != 0 ||
        b_report.qp_num == 0 || b_report.qp_num == a_report.qp_num ||
        b_report.state != UGDR_QPS_INIT) {
        return finish(8);
    }
    const std::uint32_t old_a_qp_num = a_report.qp_num;
    if (!send_command(a.commands[1], CommandCode::connect, b_report.qp_num) ||
        !read_all(a.reports[0], &a_report, sizeof(a_report)) || a_report.status != 0 ||
        a_report.state != UGDR_QPS_RTS || !send_command(b.commands[1], CommandCode::query_state) ||
        !read_all(b.reports[0], &b_report, sizeof(b_report)) || b_report.status != 0 ||
        b_report.state != UGDR_QPS_INIT) {
        return finish(9);
    }
    if (!send_command(b.commands[1], CommandCode::connect, old_a_qp_num) ||
        !read_all(b.reports[0], &b_report, sizeof(b_report)) || b_report.status != 0 ||
        b_report.state != UGDR_QPS_RTS || !send_command(a.commands[1], CommandCode::query_state) ||
        !read_all(a.reports[0], &a_report, sizeof(a_report)) || a_report.status != 0 ||
        a_report.state != UGDR_QPS_RTS) {
        return finish(10);
    }

    if (!send_command(a.commands[1], CommandCode::disconnect)) {
        return finish(11);
    }
    int a_status = 0;
    if (::waitpid(client_a, &a_status, 0) != client_a || !WIFEXITED(a_status) ||
        WEXITSTATUS(a_status) != 0) {
        return finish(12);
    }
    client_a = -1;
    (void)::poll(nullptr, 0, 300);

    if (!send_command(b.commands[1], CommandCode::verify_peer_disconnect, old_a_qp_num) ||
        !read_all(b.reports[0], &b_report, sizeof(b_report)) || b_report.status != 0 ||
        b_report.qp_num == 0 || b_report.qp_num == old_a_qp_num || b_report.state != UGDR_QPS_RTS) {
        return finish(13);
    }
    int b_status = 0;
    if (::waitpid(client_b, &b_status, 0) != client_b || !WIFEXITED(b_status) ||
        WEXITSTATUS(b_status) != 0) {
        return finish(14);
    }
    client_b = -1;

    if (::kill(daemon, SIGTERM) != 0) {
        return finish(15);
    }
    int daemon_status = 0;
    if (::waitpid(daemon, &daemon_status, 0) != daemon || !WIFEXITED(daemon_status) ||
        WEXITSTATUS(daemon_status) != 0) {
        return finish(16);
    }
    daemon = -1;
    return finish(0);
}

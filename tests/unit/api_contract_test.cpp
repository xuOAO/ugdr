#include "ugdr/api.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <unistd.h>

namespace {

template <typename T> T *sentinel_pointer(std::uintptr_t value) {
    return reinterpret_cast<T *>(value);
}

template <typename T> bool unchanged(const T &actual, const T &expected) {
    static_assert(std::is_trivially_copyable_v<T>);
    return std::memcmp(&actual, &expected, sizeof(T)) == 0;
}

}  // namespace

int main() {
    static_assert(
        std::is_same_v<decltype(&ugdr_get_device_list), ugdr_device **(*)(int *) noexcept>);
    static_assert(
        std::is_same_v<decltype(&ugdr_free_device_list), void (*)(ugdr_device **) noexcept>);
    static_assert(
        std::is_same_v<decltype(&ugdr_open_device), ugdr_context *(*)(ugdr_device *) noexcept>);
    static_assert(std::is_same_v<decltype(&ugdr_close_device), int (*)(ugdr_context *) noexcept>);
    static_assert(std::is_same_v<decltype(&ugdr_alloc_pd), ugdr_pd *(*)(ugdr_context *) noexcept>);
    static_assert(std::is_same_v<decltype(&ugdr_dealloc_pd), int (*)(ugdr_pd *) noexcept>);
    static_assert(std::is_same_v<decltype(&ugdr_reg_mr),
                                 ugdr_mr *(*)(ugdr_pd *, void *, std::size_t, int) noexcept>);
    static_assert(std::is_same_v<decltype(&ugdr_dereg_mr), int (*)(ugdr_mr *) noexcept>);
    static_assert(
        std::is_same_v<decltype(&ugdr_create_cq), ugdr_cq *(*)(ugdr_context *, int, void *,
                                                               ugdr_comp_channel *, int) noexcept>);
    static_assert(std::is_same_v<decltype(&ugdr_destroy_cq), int (*)(ugdr_cq *) noexcept>);
    static_assert(
        std::is_same_v<decltype(&ugdr_poll_cq), int (*)(ugdr_cq *, int, ugdr_wc *) noexcept>);
    static_assert(std::is_same_v<decltype(&ugdr_create_qp),
                                 ugdr_qp *(*)(ugdr_pd *, ugdr_qp_init_attr *) noexcept>);
    static_assert(std::is_same_v<decltype(&ugdr_destroy_qp), int (*)(ugdr_qp *) noexcept>);
    static_assert(std::is_same_v<decltype(&ugdr_modify_qp),
                                 int (*)(ugdr_qp *, ugdr_qp_attr *, int) noexcept>);
    static_assert(std::is_same_v<decltype(&ugdr_query_qp), int (*)(ugdr_qp *, ugdr_qp_attr *, int,
                                                                   ugdr_qp_init_attr *) noexcept>);
    static_assert(std::is_same_v<decltype(&ugdr_query_qp_conn_info),
                                 int (*)(ugdr_qp *, ugdr_qp_conn_info *) noexcept>);
    static_assert(
        std::is_same_v<decltype(&ugdr_connect_qp), int (*)(ugdr_qp *, const ugdr_qp_conn_info *,
                                                           const ugdr_qp_attr *, int) noexcept>);
    static_assert(std::is_same_v<decltype(&ugdr_post_send),
                                 int (*)(ugdr_qp *, ugdr_send_wr *, ugdr_send_wr **) noexcept>);
    static_assert(std::is_same_v<decltype(&ugdr_post_recv),
                                 int (*)(ugdr_qp *, ugdr_recv_wr *, ugdr_recv_wr **) noexcept>);

    static_assert(std::is_standard_layout_v<ugdr_mr>);
    static_assert(std::is_standard_layout_v<ugdr_sge>);
    static_assert(std::is_standard_layout_v<ugdr_send_wr>);
    static_assert(std::is_standard_layout_v<ugdr_recv_wr>);
    static_assert(std::is_standard_layout_v<ugdr_wc>);
    static_assert(std::is_standard_layout_v<ugdr_qp_init_attr>);
    static_assert(std::is_standard_layout_v<ugdr_qp_attr>);
    static_assert(std::is_standard_layout_v<ugdr_qp_conn_info>);

    const std::string socket_path =
        "/tmp/ugdr-no-daemon-" + std::to_string(static_cast<long long>(::getpid())) + ".sock";
    if (::setenv("UGDR_DAEMON_SOCKET", socket_path.c_str(), 1) != 0) {
        return 1;
    }
    int num_devices = 17;
    errno = 0;
    if (ugdr_get_device_list(&num_devices) != nullptr || errno == 0 || num_devices != 17) {
        return 2;
    }

    auto **device_list = sentinel_pointer<ugdr_device *>(1);
    errno = 0;
    ugdr_free_device_list(device_list);
    if (errno != EINVAL) {
        return 3;
    }

    errno = 0;
    if (ugdr_open_device(sentinel_pointer<ugdr_device>(2)) != nullptr || errno != EINVAL) {
        return 4;
    }
    errno = 0;
    if (ugdr_close_device(sentinel_pointer<ugdr_context>(3)) != -1 || errno != EINVAL) {
        return 5;
    }

    errno = 0;
    if (ugdr_alloc_pd(sentinel_pointer<ugdr_context>(4)) != nullptr || errno != EINVAL) {
        return 5;
    }
    errno = 101;
    if (ugdr_dealloc_pd(sentinel_pointer<ugdr_pd>(5)) != EINVAL || errno != 101) {
        return 6;
    }

    std::uint64_t memory = UINT64_C(0x1122334455667788);
    errno = 0;
    if (ugdr_reg_mr(sentinel_pointer<ugdr_pd>(6), &memory, sizeof(memory),
                    UGDR_ACCESS_LOCAL_WRITE | UGDR_ACCESS_REMOTE_WRITE) != nullptr ||
        errno != EINVAL || memory != UINT64_C(0x1122334455667788)) {
        return 7;
    }
    errno = 103;
    if (ugdr_dereg_mr(sentinel_pointer<ugdr_mr>(7)) != EINVAL || errno != 103) {
        return 8;
    }

    errno = 0;
    if (ugdr_create_cq(sentinel_pointer<ugdr_context>(8), 23, sentinel_pointer<void>(9), nullptr,
                       0) != nullptr ||
        errno != EINVAL) {
        return 9;
    }
    errno = 107;
    if (ugdr_destroy_cq(sentinel_pointer<ugdr_cq>(10)) != EINVAL || errno != 107) {
        return 10;
    }

    ugdr_wc wc{};
    wc.wr_id = 29;
    wc.status = UGDR_WC_GENERAL_ERR;
    wc.imm_data = UINT32_C(0xaabbccdd);
    const ugdr_wc expected_wc = wc;
    errno = 109;
    if (ugdr_poll_cq(sentinel_pointer<ugdr_cq>(11), 1, &wc) != -EINVAL || errno != 109 ||
        !unchanged(wc, expected_wc)) {
        return 11;
    }

    auto *const send_cq = sentinel_pointer<ugdr_cq>(12);
    auto *const recv_cq = sentinel_pointer<ugdr_cq>(13);
    ugdr_qp_init_attr init_attr{send_cq, recv_cq, 17, 19, 3, 5, UGDR_QPT_RC, 1};
    const ugdr_qp_init_attr expected_init_attr = init_attr;
    errno = 0;
    if (ugdr_create_qp(sentinel_pointer<ugdr_pd>(14), &init_attr) != nullptr || errno != EINVAL ||
        !unchanged(init_attr, expected_init_attr)) {
        return 12;
    }
    errno = 113;
    if (ugdr_destroy_qp(sentinel_pointer<ugdr_qp>(15)) != EINVAL || errno != 113) {
        return 13;
    }

    ugdr_qp_attr attr{UGDR_QPS_RTS, UGDR_QPS_INIT, UGDR_ACCESS_REMOTE_WRITE, 17, 3, 7, 19};
    const ugdr_qp_attr expected_attr = attr;
    errno = 127;
    if (ugdr_modify_qp(sentinel_pointer<ugdr_qp>(16), &attr,
                       UGDR_QP_STATE | UGDR_QP_CUR_STATE | UGDR_QP_ACCESS_FLAGS) != EINVAL ||
        errno != 127 || !unchanged(attr, expected_attr)) {
        return 14;
    }

    ugdr_qp_init_attr query_init{recv_cq, send_cq, 23, 29, 7, 11, UGDR_QPT_RC, 0};
    const ugdr_qp_init_attr expected_query_init = query_init;
    errno = 131;
    if (ugdr_query_qp(sentinel_pointer<ugdr_qp>(17), &attr, UGDR_QP_STATE, &query_init) != EINVAL ||
        errno != 131 || !unchanged(attr, expected_attr) ||
        !unchanged(query_init, expected_query_init)) {
        return 15;
    }

    ugdr_qp_conn_info info{31};
    const ugdr_qp_conn_info expected_info = info;
    errno = 137;
    if (ugdr_query_qp_conn_info(sentinel_pointer<ugdr_qp>(18), &info) != EINVAL || errno != 137 ||
        !unchanged(info, expected_info)) {
        return 16;
    }
    constexpr int connect_mask =
        UGDR_QP_TIMEOUT | UGDR_QP_RETRY_CNT | UGDR_QP_RNR_RETRY | UGDR_QP_MIN_RNR_TIMER;
    errno = 139;
    if (ugdr_connect_qp(sentinel_pointer<ugdr_qp>(19), &info, &attr, connect_mask) != EINVAL ||
        errno != 139 || !unchanged(info, expected_info) || !unchanged(attr, expected_attr)) {
        return 17;
    }

    ugdr_mr mr{nullptr, nullptr, &memory, sizeof(memory), 13, 17, 19};
    ugdr_sge sge{reinterpret_cast<std::uint64_t>(&memory), sizeof(memory), mr.lkey};
    ugdr_send_wr send_wr{};
    send_wr.wr_id = 37;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.opcode = UGDR_WR_RDMA_WRITE_WITH_IMM;
    send_wr.send_flags = UGDR_SEND_SIGNALED;
    send_wr.imm_data = UINT32_C(0xaabbccdd);
    send_wr.wr.rdma.remote_addr = UINT64_C(0xfedcba9876543210);
    send_wr.wr.rdma.rkey = mr.rkey;
    const ugdr_send_wr expected_send_wr = send_wr;
    auto *bad_send = sentinel_pointer<ugdr_send_wr>(20);
    errno = 149;
    if (ugdr_post_send(sentinel_pointer<ugdr_qp>(21), &send_wr, &bad_send) != EOPNOTSUPP ||
        errno != 149 || bad_send != sentinel_pointer<ugdr_send_wr>(20) ||
        !unchanged(send_wr, expected_send_wr)) {
        return 18;
    }

    ugdr_recv_wr recv_wr{41, nullptr, nullptr, 0};
    const ugdr_recv_wr expected_recv_wr = recv_wr;
    auto *bad_recv = sentinel_pointer<ugdr_recv_wr>(22);
    errno = 151;
    if (ugdr_post_recv(sentinel_pointer<ugdr_qp>(23), &recv_wr, &bad_recv) != EOPNOTSUPP ||
        errno != 151 || bad_recv != sentinel_pointer<ugdr_recv_wr>(22) ||
        !unchanged(recv_wr, expected_recv_wr)) {
        return 19;
    }

    return mr.lkey == 17 && mr.rkey == 19 && sge.lkey == mr.lkey && send_wr.wr.rdma.rkey == mr.rkey
               ? 0
               : 20;
}

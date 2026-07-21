#define _POSIX_C_SOURCE 200809L

#include "ugdr/api.hpp"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    int num_devices = 17;
    struct ugdr_qp_init_attr init_attr = {0};
    struct ugdr_qp_attr attr = {0};
    struct ugdr_qp_conn_info info = {23};
    struct ugdr_mr mr = {0};
    struct ugdr_sge sge = {0};
    struct ugdr_send_wr send_wr = {0};
    struct ugdr_recv_wr recv_wr = {0};
    struct ugdr_wc wc = {0};
    struct ugdr_wc expected_wc;
    struct ugdr_send_wr *bad_send = (struct ugdr_send_wr *)(uintptr_t)1;
    struct ugdr_recv_wr *bad_recv = (struct ugdr_recv_wr *)(uintptr_t)2;
    const char socket_path[] = "/tmp/ugdr-api-contract-no-daemon.sock";
    if (setenv("UGDR_DAEMON_SOCKET", socket_path, 1) != 0) {
        return 1;
    }

    mr.lkey = UINT32_C(0x11223344);
    mr.rkey = UINT32_C(0x55667788);
    sge.lkey = mr.lkey;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.opcode = UGDR_WR_RDMA_WRITE_WITH_IMM;
    send_wr.send_flags = UGDR_SEND_SIGNALED;
    send_wr.imm_data = UINT32_C(0xaabbccdd);
    send_wr.wr.rdma.remote_addr = UINT64_C(0x123456789abcdef0);
    send_wr.wr.rdma.rkey = mr.rkey;
    wc.wr_id = 29;
    wc.imm_data = send_wr.imm_data;
    wc.wc_flags = UGDR_WC_WITH_IMM;
    expected_wc = wc;

    errno = 0;
    if (ugdr_get_device_list(&num_devices) != 0 || errno == 0 || num_devices != 17) {
        return 2;
    }
    errno = 0;
    ugdr_free_device_list(0);
    if (errno != EINVAL) {
        return 3;
    }
    errno = 0;
    if (ugdr_open_device(0) != 0 || errno != EINVAL) {
        return 4;
    }
    errno = 0;
    if (ugdr_close_device(0) != -1 || errno != EINVAL) {
        return 5;
    }
    errno = 0;
    if (ugdr_alloc_pd(0) != 0 || errno != EINVAL) {
        return 6;
    }
    if (ugdr_dealloc_pd(0) != EINVAL || ugdr_reg_mr(0, 0, 0, 0) != 0 || errno != EINVAL ||
        ugdr_dereg_mr(0) != EINVAL) {
        return 7;
    }
    if (ugdr_create_cq(0, 0, 0, 0, 0) != 0 || errno != EINVAL || ugdr_destroy_cq(0) != EINVAL ||
        ugdr_poll_cq(0, 1, &wc) != -EINVAL || memcmp(&wc, &expected_wc, sizeof(wc)) != 0) {
        return 8;
    }
    errno = 0;
    if (ugdr_create_qp(0, &init_attr) != 0 || errno != EINVAL || ugdr_destroy_qp(0) != EINVAL ||
        ugdr_modify_qp(0, &attr, UGDR_QP_STATE) != EINVAL ||
        ugdr_query_qp(0, &attr, UGDR_QP_STATE, &init_attr) != EINVAL ||
        ugdr_query_qp_conn_info(0, &info) != EINVAL ||
        ugdr_connect_qp(0, &info, &attr, UGDR_QP_TIMEOUT) != EINVAL) {
        return 9;
    }
    if (ugdr_post_send(0, &send_wr, &bad_send) != EOPNOTSUPP ||
        bad_send != (struct ugdr_send_wr *)(uintptr_t)1 ||
        ugdr_post_recv(0, &recv_wr, &bad_recv) != EOPNOTSUPP ||
        bad_recv != (struct ugdr_recv_wr *)(uintptr_t)2) {
        return 10;
    }

    return sge.lkey == mr.lkey && send_wr.wr.rdma.rkey == mr.rkey && recv_wr.sg_list == 0 &&
                   wc.imm_data == send_wr.imm_data
               ? 0
               : 11;
}

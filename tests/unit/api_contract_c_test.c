#include "ugdr/api.hpp"

#include <stdint.h>

int ugdr_c_api_contract(void) {
    struct ugdr_mr mr = {0};
    struct ugdr_sge sge = {0};
    struct ugdr_send_wr send_wr = {0};
    struct ugdr_recv_wr recv_wr = {0};
    struct ugdr_wc wc = {0};

    mr.lkey = UINT32_C(0x11223344);
    mr.rkey = UINT32_C(0x55667788);
    sge.lkey = mr.lkey;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.imm_data = UINT32_C(0xaabbccdd);
    send_wr.wr.rdma.remote_addr = UINT64_C(0x123456789abcdef0);
    send_wr.wr.rdma.rkey = mr.rkey;
    recv_wr.num_sge = 0;
    recv_wr.sg_list = 0;
    wc.imm_data = send_wr.imm_data;
    wc.wc_flags = UGDR_WC_WITH_IMM;

    return sge.lkey == mr.lkey && send_wr.wr.rdma.rkey == mr.rkey && recv_wr.sg_list == 0 &&
                   wc.imm_data == send_wr.imm_data
               ? 0
               : 1;
}

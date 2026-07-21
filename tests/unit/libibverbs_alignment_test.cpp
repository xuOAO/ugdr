#include "ugdr/api.hpp"

#include <infiniband/verbs.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace {

template <typename Left, typename Right>
constexpr bool compatible_object_pointer = std::is_pointer_v<Left> &&std::is_pointer_v<Right> &&
                                           sizeof(Left) == sizeof(Right) &&
                                           alignof(Left) == alignof(Right);

template <typename Left, typename Right>
constexpr bool compatible_enum = std::is_enum_v<Left> &&std::is_enum_v<Right>
    &&std::is_same_v<std::underlying_type_t<Left>, std::underlying_type_t<Right>>;

#define UGDR_ASSERT_OFFSET(ugdr_type, ugdr_field, ibv_type, ibv_field)                             \
    static_assert(offsetof(ugdr_type, ugdr_field) == offsetof(ibv_type, ibv_field))

}  // namespace

int main() {
    static_assert(sizeof(ugdr_mr) == sizeof(ibv_mr));
    static_assert(alignof(ugdr_mr) == alignof(ibv_mr));
    static_assert(compatible_object_pointer<decltype(ugdr_mr::context), decltype(ibv_mr::context)>);
    static_assert(compatible_object_pointer<decltype(ugdr_mr::pd), decltype(ibv_mr::pd)>);
    static_assert(std::is_same_v<decltype(ugdr_mr::addr), decltype(ibv_mr::addr)>);
    static_assert(std::is_same_v<decltype(ugdr_mr::length), decltype(ibv_mr::length)>);
    static_assert(std::is_same_v<decltype(ugdr_mr::handle), decltype(ibv_mr::handle)>);
    static_assert(std::is_same_v<decltype(ugdr_mr::lkey), decltype(ibv_mr::lkey)>);
    static_assert(std::is_same_v<decltype(ugdr_mr::rkey), decltype(ibv_mr::rkey)>);
    UGDR_ASSERT_OFFSET(ugdr_mr, context, ibv_mr, context);
    UGDR_ASSERT_OFFSET(ugdr_mr, pd, ibv_mr, pd);
    UGDR_ASSERT_OFFSET(ugdr_mr, addr, ibv_mr, addr);
    UGDR_ASSERT_OFFSET(ugdr_mr, length, ibv_mr, length);
    UGDR_ASSERT_OFFSET(ugdr_mr, handle, ibv_mr, handle);
    UGDR_ASSERT_OFFSET(ugdr_mr, lkey, ibv_mr, lkey);
    UGDR_ASSERT_OFFSET(ugdr_mr, rkey, ibv_mr, rkey);

    static_assert(sizeof(ugdr_sge) == sizeof(ibv_sge));
    static_assert(alignof(ugdr_sge) == alignof(ibv_sge));
    static_assert(std::is_same_v<decltype(ugdr_sge::addr), decltype(ibv_sge::addr)>);
    static_assert(std::is_same_v<decltype(ugdr_sge::length), decltype(ibv_sge::length)>);
    static_assert(std::is_same_v<decltype(ugdr_sge::lkey), decltype(ibv_sge::lkey)>);
    UGDR_ASSERT_OFFSET(ugdr_sge, addr, ibv_sge, addr);
    UGDR_ASSERT_OFFSET(ugdr_sge, length, ibv_sge, length);
    UGDR_ASSERT_OFFSET(ugdr_sge, lkey, ibv_sge, lkey);

    static_assert(sizeof(ugdr_recv_wr) == sizeof(ibv_recv_wr));
    static_assert(alignof(ugdr_recv_wr) == alignof(ibv_recv_wr));
    static_assert(std::is_same_v<decltype(ugdr_recv_wr::wr_id), decltype(ibv_recv_wr::wr_id)>);
    static_assert(
        compatible_object_pointer<decltype(ugdr_recv_wr::next), decltype(ibv_recv_wr::next)>);
    static_assert(
        compatible_object_pointer<decltype(ugdr_recv_wr::sg_list), decltype(ibv_recv_wr::sg_list)>);
    static_assert(std::is_same_v<decltype(ugdr_recv_wr::num_sge), decltype(ibv_recv_wr::num_sge)>);
    UGDR_ASSERT_OFFSET(ugdr_recv_wr, wr_id, ibv_recv_wr, wr_id);
    UGDR_ASSERT_OFFSET(ugdr_recv_wr, next, ibv_recv_wr, next);
    UGDR_ASSERT_OFFSET(ugdr_recv_wr, sg_list, ibv_recv_wr, sg_list);
    UGDR_ASSERT_OFFSET(ugdr_recv_wr, num_sge, ibv_recv_wr, num_sge);

    static_assert(std::is_same_v<decltype(ugdr_send_wr::wr_id), decltype(ibv_send_wr::wr_id)>);
    static_assert(
        compatible_object_pointer<decltype(ugdr_send_wr::next), decltype(ibv_send_wr::next)>);
    static_assert(
        compatible_object_pointer<decltype(ugdr_send_wr::sg_list), decltype(ibv_send_wr::sg_list)>);
    static_assert(std::is_same_v<decltype(ugdr_send_wr::num_sge), decltype(ibv_send_wr::num_sge)>);
    static_assert(compatible_enum<decltype(ugdr_send_wr::opcode), decltype(ibv_send_wr::opcode)>);
    static_assert(
        std::is_same_v<decltype(ugdr_send_wr::send_flags), decltype(ibv_send_wr::send_flags)>);
    static_assert(
        std::is_same_v<decltype(ugdr_send_wr::imm_data), decltype(ibv_send_wr::imm_data)>);
    static_assert(std::is_same_v<decltype(ugdr_send_wr{}.wr.rdma.remote_addr),
                                 decltype(ibv_send_wr{}.wr.rdma.remote_addr)>);
    static_assert(std::is_same_v<decltype(ugdr_send_wr{}.wr.rdma.rkey),
                                 decltype(ibv_send_wr{}.wr.rdma.rkey)>);
    UGDR_ASSERT_OFFSET(ugdr_send_wr, wr_id, ibv_send_wr, wr_id);
    UGDR_ASSERT_OFFSET(ugdr_send_wr, next, ibv_send_wr, next);
    UGDR_ASSERT_OFFSET(ugdr_send_wr, sg_list, ibv_send_wr, sg_list);
    UGDR_ASSERT_OFFSET(ugdr_send_wr, num_sge, ibv_send_wr, num_sge);
    UGDR_ASSERT_OFFSET(ugdr_send_wr, opcode, ibv_send_wr, opcode);
    UGDR_ASSERT_OFFSET(ugdr_send_wr, send_flags, ibv_send_wr, send_flags);
    UGDR_ASSERT_OFFSET(ugdr_send_wr, imm_data, ibv_send_wr, imm_data);
    UGDR_ASSERT_OFFSET(ugdr_send_wr, wr.rdma.remote_addr, ibv_send_wr, wr.rdma.remote_addr);
    UGDR_ASSERT_OFFSET(ugdr_send_wr, wr.rdma.rkey, ibv_send_wr, wr.rdma.rkey);

    static_assert(std::is_same_v<decltype(ugdr_wc::wr_id), decltype(ibv_wc::wr_id)>);
    static_assert(compatible_enum<decltype(ugdr_wc::status), decltype(ibv_wc::status)>);
    static_assert(compatible_enum<decltype(ugdr_wc::opcode), decltype(ibv_wc::opcode)>);
    static_assert(std::is_same_v<decltype(ugdr_wc::vendor_err), decltype(ibv_wc::vendor_err)>);
    static_assert(std::is_same_v<decltype(ugdr_wc::byte_len), decltype(ibv_wc::byte_len)>);
    static_assert(std::is_same_v<decltype(ugdr_wc::imm_data), decltype(ibv_wc::imm_data)>);
    static_assert(std::is_same_v<decltype(ugdr_wc::qp_num), decltype(ibv_wc::qp_num)>);
    static_assert(std::is_same_v<decltype(ugdr_wc::src_qp), decltype(ibv_wc::src_qp)>);
    static_assert(std::is_same_v<decltype(ugdr_wc::wc_flags), decltype(ibv_wc::wc_flags)>);
    static_assert(std::is_same_v<decltype(ugdr_wc::pkey_index), decltype(ibv_wc::pkey_index)>);
    static_assert(std::is_same_v<decltype(ugdr_wc::slid), decltype(ibv_wc::slid)>);
    static_assert(std::is_same_v<decltype(ugdr_wc::sl), decltype(ibv_wc::sl)>);
    static_assert(
        std::is_same_v<decltype(ugdr_wc::dlid_path_bits), decltype(ibv_wc::dlid_path_bits)>);
    UGDR_ASSERT_OFFSET(ugdr_wc, wr_id, ibv_wc, wr_id);
    UGDR_ASSERT_OFFSET(ugdr_wc, status, ibv_wc, status);
    UGDR_ASSERT_OFFSET(ugdr_wc, opcode, ibv_wc, opcode);
    UGDR_ASSERT_OFFSET(ugdr_wc, vendor_err, ibv_wc, vendor_err);
    UGDR_ASSERT_OFFSET(ugdr_wc, byte_len, ibv_wc, byte_len);
    UGDR_ASSERT_OFFSET(ugdr_wc, imm_data, ibv_wc, imm_data);
    UGDR_ASSERT_OFFSET(ugdr_wc, qp_num, ibv_wc, qp_num);
    UGDR_ASSERT_OFFSET(ugdr_wc, src_qp, ibv_wc, src_qp);
    UGDR_ASSERT_OFFSET(ugdr_wc, wc_flags, ibv_wc, wc_flags);
    UGDR_ASSERT_OFFSET(ugdr_wc, pkey_index, ibv_wc, pkey_index);
    UGDR_ASSERT_OFFSET(ugdr_wc, slid, ibv_wc, slid);
    UGDR_ASSERT_OFFSET(ugdr_wc, sl, ibv_wc, sl);
    UGDR_ASSERT_OFFSET(ugdr_wc, dlid_path_bits, ibv_wc, dlid_path_bits);

    static_assert(
        compatible_enum<decltype(ugdr_qp_attr::qp_state), decltype(ibv_qp_attr::qp_state)>);
    static_assert(
        compatible_enum<decltype(ugdr_qp_attr::cur_qp_state), decltype(ibv_qp_attr::cur_qp_state)>);
    static_assert(std::is_same_v<decltype(ugdr_qp_attr::qp_access_flags), int>);
    static_assert(std::is_same_v<decltype(ibv_qp_attr::qp_access_flags), unsigned int>);
    static_assert(sizeof(decltype(ugdr_qp_attr::qp_access_flags)) ==
                  sizeof(decltype(ibv_qp_attr::qp_access_flags)));
    static_assert(std::is_same_v<decltype(ugdr_qp_attr::timeout), decltype(ibv_qp_attr::timeout)>);
    static_assert(
        std::is_same_v<decltype(ugdr_qp_attr::retry_cnt), decltype(ibv_qp_attr::retry_cnt)>);
    static_assert(
        std::is_same_v<decltype(ugdr_qp_attr::rnr_retry), decltype(ibv_qp_attr::rnr_retry)>);
    static_assert(std::is_same_v<decltype(ugdr_qp_attr::min_rnr_timer),
                                 decltype(ibv_qp_attr::min_rnr_timer)>);

    static_assert(UGDR_QPT_RC == IBV_QPT_RC);
    static_assert(UGDR_QPS_RESET == IBV_QPS_RESET);
    static_assert(UGDR_QPS_INIT == IBV_QPS_INIT);
    static_assert(UGDR_QPS_RTR == IBV_QPS_RTR);
    static_assert(UGDR_QPS_RTS == IBV_QPS_RTS);
    static_assert(UGDR_QPS_SQD == IBV_QPS_SQD);
    static_assert(UGDR_QPS_SQE == IBV_QPS_SQE);
    static_assert(UGDR_QPS_ERR == IBV_QPS_ERR);
    static_assert(UGDR_QPS_UNKNOWN == IBV_QPS_UNKNOWN);
    static_assert(UGDR_QP_STATE == IBV_QP_STATE);
    static_assert(UGDR_QP_CUR_STATE == IBV_QP_CUR_STATE);
    static_assert(UGDR_QP_ACCESS_FLAGS == IBV_QP_ACCESS_FLAGS);
    static_assert(UGDR_QP_TIMEOUT == IBV_QP_TIMEOUT);
    static_assert(UGDR_QP_RETRY_CNT == IBV_QP_RETRY_CNT);
    static_assert(UGDR_QP_RNR_RETRY == IBV_QP_RNR_RETRY);
    static_assert(UGDR_QP_MIN_RNR_TIMER == IBV_QP_MIN_RNR_TIMER);
    static_assert(UGDR_WR_RDMA_WRITE == IBV_WR_RDMA_WRITE);
    static_assert(UGDR_WR_RDMA_WRITE_WITH_IMM == IBV_WR_RDMA_WRITE_WITH_IMM);
    static_assert(UGDR_SEND_SIGNALED == IBV_SEND_SIGNALED);
    static_assert(UGDR_WC_SUCCESS == IBV_WC_SUCCESS);
    static_assert(UGDR_WC_LOC_LEN_ERR == IBV_WC_LOC_LEN_ERR);
    static_assert(UGDR_WC_LOC_QP_OP_ERR == IBV_WC_LOC_QP_OP_ERR);
    static_assert(UGDR_WC_LOC_PROT_ERR == IBV_WC_LOC_PROT_ERR);
    static_assert(UGDR_WC_WR_FLUSH_ERR == IBV_WC_WR_FLUSH_ERR);
    static_assert(UGDR_WC_LOC_ACCESS_ERR == IBV_WC_LOC_ACCESS_ERR);
    static_assert(UGDR_WC_REM_INV_REQ_ERR == IBV_WC_REM_INV_REQ_ERR);
    static_assert(UGDR_WC_REM_ACCESS_ERR == IBV_WC_REM_ACCESS_ERR);
    static_assert(UGDR_WC_REM_OP_ERR == IBV_WC_REM_OP_ERR);
    static_assert(UGDR_WC_RETRY_EXC_ERR == IBV_WC_RETRY_EXC_ERR);
    static_assert(UGDR_WC_RNR_RETRY_EXC_ERR == IBV_WC_RNR_RETRY_EXC_ERR);
    static_assert(UGDR_WC_GENERAL_ERR == IBV_WC_GENERAL_ERR);
    static_assert(UGDR_WC_RDMA_WRITE == IBV_WC_RDMA_WRITE);
    static_assert(UGDR_WC_RECV_RDMA_WITH_IMM == IBV_WC_RECV_RDMA_WITH_IMM);
    static_assert(UGDR_WC_WITH_IMM == IBV_WC_WITH_IMM);
    static_assert(UGDR_ACCESS_LOCAL_WRITE == IBV_ACCESS_LOCAL_WRITE);
    static_assert(UGDR_ACCESS_REMOTE_WRITE == IBV_ACCESS_REMOTE_WRITE);

    return 0;
}

#undef UGDR_ASSERT_OFFSET

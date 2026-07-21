#include "ugdr/api.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>

extern "C" int ugdr_c_api_contract(void);

namespace {

bool contains_all_public_symbols() {
    const std::string alignment_path =
        std::string(UGDR_SOURCE_DIR) + "/docs/contracts/libibverbs-alignment.md";
    std::ifstream input(alignment_path);
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    const char *symbols[] = {
        "ugdr_get_device_list", "ugdr_free_device_list",
        "ugdr_open_device",     "ugdr_close_device",
        "ugdr_alloc_pd",        "ugdr_dealloc_pd",
        "ugdr_reg_mr",          "ugdr_dereg_mr",
        "ugdr_create_cq",       "ugdr_destroy_cq",
        "ugdr_poll_cq",         "ugdr_create_qp",
        "ugdr_destroy_qp",      "ugdr_modify_qp",
        "ugdr_query_qp",        "ugdr_query_qp_conn_info",
        "ugdr_connect_qp",      "ugdr_post_send",
        "ugdr_post_recv",
    };
    if (!input) {
        return false;
    }
    for (const char *symbol : symbols) {
        if (contents.find(symbol) == std::string::npos) {
            return false;
        }
    }
    return true;
}

bool contains_qp_state_contract() {
    const std::string contract_path =
        std::string(UGDR_SOURCE_DIR) + "/docs/contracts/rc-qp-state-machine.md";
    std::ifstream input(contract_path);
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    const char *markers[] = {
        "UGDR_QP_STATE",
        "UGDR_QP_CUR_STATE",
        "UGDR_QP_ACCESS_FLAGS",
        "ugdr_qp_conn_info",
        "qp_num",
        "endpoint_id",
        "UGDR_QPS_RESET",
        "UGDR_QPS_INIT",
        "UGDR_QPS_RTR",
        "UGDR_QPS_RTS",
        "UGDR_QPS_ERR",
        "EOPNOTSUPP",
        "EINVAL",
        "ENOENT",
        "EBUSY",
        "`UGDR_QPS_RESET` | `UGDR_QPS_INIT`",
        "`UGDR_QPS_INIT` | `UGDR_QPS_RTS`",
        "RESET, INIT, RTR, or RTS | `UGDR_QPS_ERR`",
        "`UGDR_QPS_SQD` or `UGDR_QPS_SQE`",
        "repeated call returns `EINVAL`",
        "All failures are atomic.",
        "not a network or persistent wire format",
    };
    if (!input) {
        return false;
    }
    for (const char *marker : markers) {
        if (contents.find(marker) == std::string::npos) {
            return false;
        }
    }
    return true;
}

bool contains_wr_wc_contract() {
    const std::string contract_path =
        std::string(UGDR_SOURCE_DIR) + "/docs/contracts/wr-wc-semantics.md";
    std::ifstream input(contract_path);
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    const char *markers[] = {
        "mr->lkey",
        "mr->rkey",
        "wr.wr.rdma",
        "UGDR_WC_WITH_IMM",
        "UGDR_WR_RDMA_WRITE_WITH_IMM",
        "UGDR_WC_RECV_RDMA_WITH_IMM",
        "UGDR_WC_RNR_RETRY_EXC_ERR",
        "UGDR_WC_WR_FLUSH_ERR",
        "bad_wr",
        "EOPNOTSUPP",
        "EBUSY",
    };
    if (!input) {
        return false;
    }
    for (const char *marker : markers) {
        if (contents.find(marker) == std::string::npos) {
            return false;
        }
    }
    return true;
}

template <typename T> T *sentinel_pointer() {
    return reinterpret_cast<T *>(static_cast<std::uintptr_t>(1));
}

}  // namespace

int main() {
    static_assert(
        std::is_same_v<decltype(&ugdr_open_device), ugdr_context *(*)(ugdr_device *) noexcept>);
    static_assert(
        std::is_same_v<decltype(&ugdr_create_cq), ugdr_cq *(*)(ugdr_context *, int, void *,
                                                               ugdr_comp_channel *, int) noexcept>);
    static_assert(std::is_same_v<decltype(&ugdr_post_send),
                                 int (*)(ugdr_qp *, ugdr_send_wr *, ugdr_send_wr **) noexcept>);
    static_assert(
        std::is_same_v<decltype(&ugdr_connect_qp), int (*)(ugdr_qp *, const ugdr_qp_conn_info *,
                                                           const ugdr_qp_attr *, int) noexcept>);
    static_assert(std::is_standard_layout_v<ugdr_mr>);
    static_assert(std::is_standard_layout_v<ugdr_sge>);
    static_assert(std::is_standard_layout_v<ugdr_send_wr>);
    static_assert(std::is_standard_layout_v<ugdr_recv_wr>);
    static_assert(std::is_standard_layout_v<ugdr_wc>);
    static_assert(std::is_standard_layout_v<ugdr_qp_init_attr>);
    static_assert(std::is_standard_layout_v<ugdr_qp_attr>);
    static_assert(std::is_standard_layout_v<ugdr_qp_conn_info>);
    static_assert(std::is_same_v<decltype(ugdr_qp_init_attr::send_cq), ugdr_cq *>);
    static_assert(std::is_same_v<decltype(ugdr_qp_init_attr::recv_cq), ugdr_cq *>);
    static_assert(std::is_same_v<decltype(ugdr_qp_init_attr::max_send_wr), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_qp_init_attr::max_recv_wr), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_qp_init_attr::max_send_sge), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_qp_init_attr::max_recv_sge), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_qp_init_attr::qp_type), ugdr_qp_type>);
    static_assert(std::is_same_v<decltype(ugdr_qp_init_attr::sq_sig_all), int>);
    static_assert(std::is_same_v<decltype(ugdr_qp_attr::qp_state), ugdr_qp_state>);
    static_assert(std::is_same_v<decltype(ugdr_qp_attr::cur_qp_state), ugdr_qp_state>);
    static_assert(std::is_same_v<decltype(ugdr_qp_attr::qp_access_flags), int>);
    static_assert(std::is_same_v<decltype(ugdr_qp_attr::timeout), std::uint8_t>);
    static_assert(std::is_same_v<decltype(ugdr_qp_attr::retry_cnt), std::uint8_t>);
    static_assert(std::is_same_v<decltype(ugdr_qp_attr::rnr_retry), std::uint8_t>);
    static_assert(std::is_same_v<decltype(ugdr_qp_attr::min_rnr_timer), std::uint8_t>);
    static_assert(std::is_same_v<decltype(ugdr_qp_conn_info::qp_num), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_qp_conn_info::endpoint_id), std::uint64_t>);
    static_assert(std::is_same_v<decltype(ugdr_mr::context), ugdr_context *>);
    static_assert(std::is_same_v<decltype(ugdr_mr::pd), ugdr_pd *>);
    static_assert(std::is_same_v<decltype(ugdr_mr::addr), void *>);
    static_assert(std::is_same_v<decltype(ugdr_mr::length), std::size_t>);
    static_assert(std::is_same_v<decltype(ugdr_mr::handle), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_mr::lkey), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_mr::rkey), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_sge::addr), std::uint64_t>);
    static_assert(std::is_same_v<decltype(ugdr_sge::length), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_sge::lkey), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_send_wr::wr_id), std::uint64_t>);
    static_assert(std::is_same_v<decltype(ugdr_send_wr::next), ugdr_send_wr *>);
    static_assert(std::is_same_v<decltype(ugdr_send_wr::sg_list), ugdr_sge *>);
    static_assert(std::is_same_v<decltype(ugdr_send_wr::num_sge), int>);
    static_assert(std::is_same_v<decltype(ugdr_send_wr::opcode), ugdr_wr_opcode>);
    static_assert(std::is_same_v<decltype(ugdr_send_wr::send_flags), unsigned int>);
    static_assert(std::is_same_v<decltype(ugdr_send_wr::imm_data), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_send_wr{}.wr.rdma.remote_addr), std::uint64_t>);
    static_assert(std::is_same_v<decltype(ugdr_send_wr{}.wr.rdma.rkey), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_recv_wr::wr_id), std::uint64_t>);
    static_assert(std::is_same_v<decltype(ugdr_recv_wr::next), ugdr_recv_wr *>);
    static_assert(std::is_same_v<decltype(ugdr_recv_wr::sg_list), ugdr_sge *>);
    static_assert(std::is_same_v<decltype(ugdr_recv_wr::num_sge), int>);
    static_assert(std::is_same_v<decltype(ugdr_wc::wr_id), std::uint64_t>);
    static_assert(std::is_same_v<decltype(ugdr_wc::status), ugdr_wc_status>);
    static_assert(std::is_same_v<decltype(ugdr_wc::opcode), ugdr_wc_opcode>);
    static_assert(std::is_same_v<decltype(ugdr_wc::vendor_err), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_wc::byte_len), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_wc::imm_data), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_wc::qp_num), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_wc::src_qp), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_wc::wc_flags), unsigned int>);
    static_assert(std::is_same_v<decltype(ugdr_wc::pkey_index), std::uint16_t>);
    static_assert(std::is_same_v<decltype(ugdr_wc::slid), std::uint16_t>);
    static_assert(std::is_same_v<decltype(ugdr_wc::sl), std::uint8_t>);
    static_assert(std::is_same_v<decltype(ugdr_wc::dlid_path_bits), std::uint8_t>);
    static_assert(offsetof(ugdr_qp_init_attr, send_cq) < offsetof(ugdr_qp_init_attr, recv_cq));
    static_assert(offsetof(ugdr_qp_init_attr, recv_cq) < offsetof(ugdr_qp_init_attr, max_send_wr));
    static_assert(offsetof(ugdr_qp_init_attr, max_recv_sge) < offsetof(ugdr_qp_init_attr, qp_type));
    static_assert(offsetof(ugdr_qp_init_attr, qp_type) < offsetof(ugdr_qp_init_attr, sq_sig_all));
    static_assert(offsetof(ugdr_qp_attr, qp_state) < offsetof(ugdr_qp_attr, cur_qp_state));
    static_assert(offsetof(ugdr_qp_attr, cur_qp_state) < offsetof(ugdr_qp_attr, qp_access_flags));
    static_assert(offsetof(ugdr_qp_attr, qp_access_flags) < offsetof(ugdr_qp_attr, timeout));
    static_assert(offsetof(ugdr_qp_attr, timeout) < offsetof(ugdr_qp_attr, retry_cnt));
    static_assert(offsetof(ugdr_qp_attr, retry_cnt) < offsetof(ugdr_qp_attr, rnr_retry));
    static_assert(offsetof(ugdr_qp_attr, rnr_retry) < offsetof(ugdr_qp_attr, min_rnr_timer));
    static_assert(offsetof(ugdr_qp_conn_info, qp_num) < offsetof(ugdr_qp_conn_info, endpoint_id));
    static_assert(offsetof(ugdr_mr, context) < offsetof(ugdr_mr, pd));
    static_assert(offsetof(ugdr_mr, pd) < offsetof(ugdr_mr, addr));
    static_assert(offsetof(ugdr_mr, addr) < offsetof(ugdr_mr, length));
    static_assert(offsetof(ugdr_mr, length) < offsetof(ugdr_mr, handle));
    static_assert(offsetof(ugdr_mr, handle) < offsetof(ugdr_mr, lkey));
    static_assert(offsetof(ugdr_mr, lkey) < offsetof(ugdr_mr, rkey));
    static_assert(offsetof(ugdr_sge, addr) < offsetof(ugdr_sge, length));
    static_assert(offsetof(ugdr_sge, length) < offsetof(ugdr_sge, lkey));
    static_assert(offsetof(ugdr_send_wr, wr_id) < offsetof(ugdr_send_wr, next));
    static_assert(offsetof(ugdr_send_wr, next) < offsetof(ugdr_send_wr, sg_list));
    static_assert(offsetof(ugdr_send_wr, sg_list) < offsetof(ugdr_send_wr, num_sge));
    static_assert(offsetof(ugdr_send_wr, num_sge) < offsetof(ugdr_send_wr, opcode));
    static_assert(offsetof(ugdr_send_wr, opcode) < offsetof(ugdr_send_wr, send_flags));
    static_assert(offsetof(ugdr_send_wr, send_flags) < offsetof(ugdr_send_wr, imm_data));
    static_assert(offsetof(ugdr_send_wr, imm_data) < offsetof(ugdr_send_wr, wr));
    static_assert(offsetof(ugdr_recv_wr, wr_id) < offsetof(ugdr_recv_wr, next));
    static_assert(offsetof(ugdr_recv_wr, next) < offsetof(ugdr_recv_wr, sg_list));
    static_assert(offsetof(ugdr_recv_wr, sg_list) < offsetof(ugdr_recv_wr, num_sge));
    static_assert(offsetof(ugdr_wc, wr_id) < offsetof(ugdr_wc, status));
    static_assert(offsetof(ugdr_wc, status) < offsetof(ugdr_wc, opcode));
    static_assert(offsetof(ugdr_wc, opcode) < offsetof(ugdr_wc, vendor_err));
    static_assert(offsetof(ugdr_wc, vendor_err) < offsetof(ugdr_wc, byte_len));
    static_assert(offsetof(ugdr_wc, byte_len) < offsetof(ugdr_wc, imm_data));
    static_assert(offsetof(ugdr_wc, imm_data) < offsetof(ugdr_wc, qp_num));
    static_assert(offsetof(ugdr_wc, qp_num) < offsetof(ugdr_wc, src_qp));
    static_assert(offsetof(ugdr_wc, src_qp) < offsetof(ugdr_wc, wc_flags));
    static_assert(offsetof(ugdr_wc, wc_flags) < offsetof(ugdr_wc, pkey_index));
    static_assert(offsetof(ugdr_wc, pkey_index) < offsetof(ugdr_wc, slid));
    static_assert(offsetof(ugdr_wc, slid) < offsetof(ugdr_wc, sl));
    static_assert(offsetof(ugdr_wc, sl) < offsetof(ugdr_wc, dlid_path_bits));
    static_assert(UGDR_QPT_RC == 2);
    static_assert(UGDR_QPS_RESET == 0);
    static_assert(UGDR_QPS_INIT == 1);
    static_assert(UGDR_QPS_RTR == 2);
    static_assert(UGDR_QPS_RTS == 3);
    static_assert(UGDR_QPS_SQD == 4);
    static_assert(UGDR_QPS_SQE == 5);
    static_assert(UGDR_QPS_ERR == 6);
    static_assert(UGDR_QPS_UNKNOWN == 7);
    static_assert(UGDR_QP_STATE == (1U << 0U));
    static_assert(UGDR_QP_CUR_STATE == (1U << 1U));
    static_assert(UGDR_QP_ACCESS_FLAGS == (1U << 3U));
    static_assert(UGDR_QP_TIMEOUT == (1U << 9U));
    static_assert(UGDR_QP_RETRY_CNT == (1U << 10U));
    static_assert(UGDR_QP_RNR_RETRY == (1U << 11U));
    static_assert(UGDR_QP_MIN_RNR_TIMER == (1U << 15U));
    static_assert(UGDR_WR_RDMA_WRITE == 0);
    static_assert(UGDR_WR_RDMA_WRITE_WITH_IMM == 1);
    static_assert(UGDR_SEND_SIGNALED == (1U << 1U));
    static_assert(UGDR_WC_SUCCESS == 0);
    static_assert(UGDR_WC_LOC_LEN_ERR == 1);
    static_assert(UGDR_WC_LOC_QP_OP_ERR == 2);
    static_assert(UGDR_WC_LOC_PROT_ERR == 4);
    static_assert(UGDR_WC_WR_FLUSH_ERR == 5);
    static_assert(UGDR_WC_LOC_ACCESS_ERR == 8);
    static_assert(UGDR_WC_REM_INV_REQ_ERR == 9);
    static_assert(UGDR_WC_REM_ACCESS_ERR == 10);
    static_assert(UGDR_WC_REM_OP_ERR == 11);
    static_assert(UGDR_WC_RETRY_EXC_ERR == 12);
    static_assert(UGDR_WC_RNR_RETRY_EXC_ERR == 13);
    static_assert(UGDR_WC_GENERAL_ERR == 21);
    static_assert(UGDR_WC_RDMA_WRITE == 1);
    static_assert(UGDR_WC_RECV_RDMA_WITH_IMM == 129);
    static_assert(UGDR_WC_WITH_IMM == (1U << 1U));
    static_assert(UGDR_ACCESS_LOCAL_WRITE == (1U << 0U));
    static_assert(UGDR_ACCESS_REMOTE_WRITE == (1U << 1U));

    int num_devices = 17;
    errno = 0;
    if (ugdr_get_device_list(&num_devices) != nullptr || errno != EOPNOTSUPP || num_devices != 17) {
        return 1;
    }

    errno = 0;
    ugdr_free_device_list(nullptr);
    if (errno != EOPNOTSUPP) {
        return 2;
    }

    errno = 0;
    if (ugdr_open_device(nullptr) != nullptr || errno != EOPNOTSUPP) {
        return 3;
    }
    errno = 0;
    if (ugdr_close_device(nullptr) != -1 || errno != EOPNOTSUPP) {
        return 4;
    }

    errno = 29;
    if (ugdr_alloc_pd(nullptr) != nullptr || errno != EOPNOTSUPP ||
        ugdr_dealloc_pd(nullptr) != EOPNOTSUPP || errno != EOPNOTSUPP) {
        return 5;
    }

    errno = 31;
    if (ugdr_reg_mr(nullptr, nullptr, 0, 0) != nullptr || errno != EOPNOTSUPP ||
        ugdr_dereg_mr(nullptr) != EOPNOTSUPP || errno != EOPNOTSUPP) {
        return 6;
    }

    errno = 37;
    if (ugdr_create_cq(nullptr, 0, nullptr, nullptr, 0) != nullptr || errno != EOPNOTSUPP ||
        ugdr_destroy_cq(nullptr) != EOPNOTSUPP || errno != EOPNOTSUPP ||
        ugdr_poll_cq(nullptr, 0, nullptr) != -EOPNOTSUPP || errno != EOPNOTSUPP) {
        return 7;
    }

    auto *const send_cq = sentinel_pointer<ugdr_cq>();
    auto *const recv_cq = reinterpret_cast<ugdr_cq *>(static_cast<std::uintptr_t>(2));
    ugdr_qp_init_attr init_attr{
        send_cq, recv_cq, 17, 19, 3, 5, UGDR_QPT_RC, 1,
    };
    ugdr_qp_attr attr{
        UGDR_QPS_RTS, UGDR_QPS_INIT, UGDR_ACCESS_REMOTE_WRITE, 17, 3, 7, 19,
    };
    ugdr_qp_init_attr queried_init_attr{
        recv_cq, send_cq, 23, 29, 7, 11, UGDR_QPT_RC, 0,
    };

    errno = 41;
    if (ugdr_create_qp(nullptr, &init_attr) != nullptr || errno != EOPNOTSUPP ||
        init_attr.send_cq != send_cq || init_attr.recv_cq != recv_cq ||
        init_attr.max_send_wr != 17 || init_attr.max_recv_wr != 19 || init_attr.max_send_sge != 3 ||
        init_attr.max_recv_sge != 5 || init_attr.qp_type != UGDR_QPT_RC ||
        init_attr.sq_sig_all != 1 || ugdr_destroy_qp(nullptr) != EOPNOTSUPP ||
        errno != EOPNOTSUPP ||
        ugdr_modify_qp(nullptr, &attr, UGDR_QP_STATE | UGDR_QP_CUR_STATE | UGDR_QP_ACCESS_FLAGS) !=
            EOPNOTSUPP ||
        attr.qp_state != UGDR_QPS_RTS || attr.cur_qp_state != UGDR_QPS_INIT ||
        attr.qp_access_flags != UGDR_ACCESS_REMOTE_WRITE || attr.timeout != 17 ||
        attr.retry_cnt != 3 || attr.rnr_retry != 7 || attr.min_rnr_timer != 19 ||
        ugdr_query_qp(nullptr, &attr, UGDR_QP_STATE, &queried_init_attr) != EOPNOTSUPP ||
        attr.qp_state != UGDR_QPS_RTS || attr.cur_qp_state != UGDR_QPS_INIT ||
        attr.qp_access_flags != UGDR_ACCESS_REMOTE_WRITE || attr.timeout != 17 ||
        attr.retry_cnt != 3 || attr.rnr_retry != 7 || attr.min_rnr_timer != 19 ||
        queried_init_attr.send_cq != recv_cq || queried_init_attr.recv_cq != send_cq ||
        queried_init_attr.max_send_wr != 23 || queried_init_attr.max_recv_wr != 29 ||
        queried_init_attr.max_send_sge != 7 || queried_init_attr.max_recv_sge != 11 ||
        queried_init_attr.qp_type != UGDR_QPT_RC || queried_init_attr.sq_sig_all != 0 ||
        errno != EOPNOTSUPP) {
        return 8;
    }

    ugdr_qp_conn_info info{31, UINT64_C(0x123456789abcdef0)};
    constexpr int connect_mask =
        UGDR_QP_TIMEOUT | UGDR_QP_RETRY_CNT | UGDR_QP_RNR_RETRY | UGDR_QP_MIN_RNR_TIMER;
    if (ugdr_query_qp_conn_info(nullptr, &info) != EOPNOTSUPP || info.qp_num != 31 ||
        info.endpoint_id != UINT64_C(0x123456789abcdef0) ||
        ugdr_connect_qp(nullptr, &info, &attr, connect_mask) != EOPNOTSUPP || info.qp_num != 31 ||
        info.endpoint_id != UINT64_C(0x123456789abcdef0) || attr.timeout != 17 ||
        attr.retry_cnt != 3 || attr.rnr_retry != 7 || attr.min_rnr_timer != 19) {
        return 9;
    }

    ugdr_mr mr{nullptr, nullptr, nullptr, 4096, 13, 17, 19};
    ugdr_sge sge{UINT64_C(0x123456789abcdef0), 1024, mr.lkey};
    ugdr_send_wr send_wr{};
    send_wr.wr_id = 23;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.opcode = UGDR_WR_RDMA_WRITE_WITH_IMM;
    send_wr.send_flags = UGDR_SEND_SIGNALED;
    send_wr.imm_data = UINT32_C(0xaabbccdd);
    send_wr.wr.rdma.remote_addr = UINT64_C(0xfedcba9876543210);
    send_wr.wr.rdma.rkey = mr.rkey;
    const ugdr_send_wr expected_send_wr = send_wr;

    ugdr_recv_wr recv_wr{29, nullptr, nullptr, 0};
    const ugdr_recv_wr expected_recv_wr = recv_wr;
    ugdr_wc wc{};
    wc.wr_id = 31;
    wc.status = UGDR_WC_SUCCESS;
    wc.opcode = UGDR_WC_RECV_RDMA_WITH_IMM;
    wc.vendor_err = 37;
    wc.byte_len = sge.length;
    wc.imm_data = send_wr.imm_data;
    wc.qp_num = 41;
    wc.src_qp = 43;
    wc.wc_flags = UGDR_WC_WITH_IMM;
    wc.pkey_index = 47;
    wc.slid = 53;
    wc.sl = 59;
    wc.dlid_path_bits = 61;
    const ugdr_wc expected_wc = wc;

    auto *bad_send = sentinel_pointer<ugdr_send_wr>();
    auto *bad_recv = sentinel_pointer<ugdr_recv_wr>();
    if (mr.lkey != 17 || mr.rkey != 19 || send_wr.wr.rdma.rkey != mr.rkey ||
        ugdr_post_send(nullptr, &send_wr, &bad_send) != EOPNOTSUPP ||
        bad_send != sentinel_pointer<ugdr_send_wr>() ||
        std::memcmp(&send_wr, &expected_send_wr, sizeof(send_wr)) != 0 ||
        ugdr_post_recv(nullptr, &recv_wr, &bad_recv) != EOPNOTSUPP ||
        bad_recv != sentinel_pointer<ugdr_recv_wr>() ||
        std::memcmp(&recv_wr, &expected_recv_wr, sizeof(recv_wr)) != 0 ||
        ugdr_poll_cq(nullptr, 1, &wc) != -EOPNOTSUPP ||
        std::memcmp(&wc, &expected_wc, sizeof(wc)) != 0) {
        return 10;
    }

    if (!contains_all_public_symbols()) {
        return 11;
    }
    if (!contains_qp_state_contract()) {
        return 12;
    }
    if (!contains_wr_wc_contract()) {
        return 13;
    }
    return ugdr_c_api_contract() == 0 ? 0 : 14;
}

#include "ugdr/api.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>

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
    static_assert(std::is_same_v<decltype(ugdr_qp_conn_info::qp_num), std::uint32_t>);
    static_assert(std::is_same_v<decltype(ugdr_qp_conn_info::endpoint_id), std::uint64_t>);
    static_assert(offsetof(ugdr_qp_init_attr, send_cq) < offsetof(ugdr_qp_init_attr, recv_cq));
    static_assert(offsetof(ugdr_qp_init_attr, recv_cq) < offsetof(ugdr_qp_init_attr, max_send_wr));
    static_assert(offsetof(ugdr_qp_init_attr, max_recv_sge) < offsetof(ugdr_qp_init_attr, qp_type));
    static_assert(offsetof(ugdr_qp_init_attr, qp_type) < offsetof(ugdr_qp_init_attr, sq_sig_all));
    static_assert(offsetof(ugdr_qp_attr, qp_state) < offsetof(ugdr_qp_attr, cur_qp_state));
    static_assert(offsetof(ugdr_qp_attr, cur_qp_state) < offsetof(ugdr_qp_attr, qp_access_flags));
    static_assert(offsetof(ugdr_qp_conn_info, qp_num) < offsetof(ugdr_qp_conn_info, endpoint_id));
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
    static_assert(UGDR_WR_RDMA_WRITE == 0);
    static_assert(UGDR_WR_RDMA_WRITE_WITH_IMM == 1);
    static_assert(UGDR_SEND_SIGNALED == (1U << 1U));
    static_assert(UGDR_WC_RDMA_WRITE == 1);
    static_assert(UGDR_WC_RECV_RDMA_WITH_IMM == 129);
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
        UGDR_QPS_RTS,
        UGDR_QPS_INIT,
        UGDR_ACCESS_REMOTE_WRITE,
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
        attr.qp_access_flags != UGDR_ACCESS_REMOTE_WRITE ||
        ugdr_query_qp(nullptr, &attr, UGDR_QP_STATE, &queried_init_attr) != EOPNOTSUPP ||
        attr.qp_state != UGDR_QPS_RTS || attr.cur_qp_state != UGDR_QPS_INIT ||
        attr.qp_access_flags != UGDR_ACCESS_REMOTE_WRITE || queried_init_attr.send_cq != recv_cq ||
        queried_init_attr.recv_cq != send_cq || queried_init_attr.max_send_wr != 23 ||
        queried_init_attr.max_recv_wr != 29 || queried_init_attr.max_send_sge != 7 ||
        queried_init_attr.max_recv_sge != 11 || queried_init_attr.qp_type != UGDR_QPT_RC ||
        queried_init_attr.sq_sig_all != 0 || errno != EOPNOTSUPP) {
        return 8;
    }

    ugdr_qp_conn_info info{31, UINT64_C(0x123456789abcdef0)};
    if (ugdr_query_qp_conn_info(nullptr, &info) != EOPNOTSUPP || info.qp_num != 31 ||
        info.endpoint_id != UINT64_C(0x123456789abcdef0) ||
        ugdr_connect_qp(nullptr, &info) != EOPNOTSUPP || info.qp_num != 31 ||
        info.endpoint_id != UINT64_C(0x123456789abcdef0)) {
        return 9;
    }

    auto *bad_send = sentinel_pointer<ugdr_send_wr>();
    auto *bad_recv = sentinel_pointer<ugdr_recv_wr>();
    if (ugdr_post_send(nullptr, nullptr, &bad_send) != EOPNOTSUPP ||
        bad_send != sentinel_pointer<ugdr_send_wr>() ||
        ugdr_post_recv(nullptr, nullptr, &bad_recv) != EOPNOTSUPP ||
        bad_recv != sentinel_pointer<ugdr_recv_wr>()) {
        return 10;
    }

    if (!contains_all_public_symbols()) {
        return 11;
    }
    return contains_qp_state_contract() ? 0 : 12;
}

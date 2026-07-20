#include "ugdr/api.hpp"

#include <cerrno>
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
    static_assert(UGDR_QPT_RC == 2);
    static_assert(UGDR_QPS_RTS == 3);
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

    errno = 41;
    if (ugdr_create_qp(nullptr, nullptr) != nullptr || errno != EOPNOTSUPP ||
        ugdr_destroy_qp(nullptr) != EOPNOTSUPP || errno != EOPNOTSUPP ||
        ugdr_modify_qp(nullptr, nullptr, 0) != EOPNOTSUPP || errno != EOPNOTSUPP ||
        ugdr_query_qp(nullptr, nullptr, 0, nullptr) != EOPNOTSUPP || errno != EOPNOTSUPP) {
        return 8;
    }

    auto *const info = sentinel_pointer<ugdr_qp_conn_info>();
    if (ugdr_query_qp_conn_info(nullptr, info) != EOPNOTSUPP ||
        ugdr_connect_qp(nullptr, info) != EOPNOTSUPP) {
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

    return contains_all_public_symbols() ? 0 : 11;
}

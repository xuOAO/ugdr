#include "ugdr/api.hpp"

#include <cerrno>

namespace {

template <typename T> T *unsupported_pointer() noexcept {
    errno = EOPNOTSUPP;
    return nullptr;
}

constexpr int kUnsupported = EOPNOTSUPP;

}  // namespace

extern "C" {

ugdr_device **ugdr_get_device_list(int *) noexcept {
    return unsupported_pointer<ugdr_device *>();
}

void ugdr_free_device_list(ugdr_device **) noexcept {
    errno = EOPNOTSUPP;
}

ugdr_context *ugdr_open_device(ugdr_device *) noexcept {
    return unsupported_pointer<ugdr_context>();
}

int ugdr_close_device(ugdr_context *) noexcept {
    errno = EOPNOTSUPP;
    return -1;
}

ugdr_pd *ugdr_alloc_pd(ugdr_context *) noexcept {
    return unsupported_pointer<ugdr_pd>();
}

int ugdr_dealloc_pd(ugdr_pd *) noexcept {
    return kUnsupported;
}

ugdr_mr *ugdr_reg_mr(ugdr_pd *, void *, size_t, int) noexcept {
    return unsupported_pointer<ugdr_mr>();
}

int ugdr_dereg_mr(ugdr_mr *) noexcept {
    return kUnsupported;
}

ugdr_cq *ugdr_create_cq(ugdr_context *, int, void *, ugdr_comp_channel *, int) noexcept {
    return unsupported_pointer<ugdr_cq>();
}

int ugdr_destroy_cq(ugdr_cq *) noexcept {
    return kUnsupported;
}

int ugdr_poll_cq(ugdr_cq *, int, ugdr_wc *) noexcept {
    return -kUnsupported;
}

ugdr_qp *ugdr_create_qp(ugdr_pd *, ugdr_qp_init_attr *) noexcept {
    return unsupported_pointer<ugdr_qp>();
}

int ugdr_destroy_qp(ugdr_qp *) noexcept {
    return kUnsupported;
}

int ugdr_modify_qp(ugdr_qp *, ugdr_qp_attr *, int) noexcept {
    return kUnsupported;
}

int ugdr_query_qp(ugdr_qp *, ugdr_qp_attr *, int, ugdr_qp_init_attr *) noexcept {
    return kUnsupported;
}

int ugdr_query_qp_conn_info(ugdr_qp *, ugdr_qp_conn_info *) noexcept {
    return kUnsupported;
}

int ugdr_connect_qp(ugdr_qp *, const ugdr_qp_conn_info *) noexcept {
    return kUnsupported;
}

int ugdr_post_send(ugdr_qp *, ugdr_send_wr *, ugdr_send_wr **) noexcept {
    return kUnsupported;
}

int ugdr_post_recv(ugdr_qp *, ugdr_recv_wr *, ugdr_recv_wr **) noexcept {
    return kUnsupported;
}

}  // extern "C"

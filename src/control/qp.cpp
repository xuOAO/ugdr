#include "control/qp.hpp"

#include <arpa/inet.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace ugdr::control {
namespace {

constexpr std::size_t kQpCreatePayloadSize = 44;

std::uint64_t host_to_network64(std::uint64_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (static_cast<std::uint64_t>(htonl(static_cast<std::uint32_t>(value))) << 32U) |
           htonl(static_cast<std::uint32_t>(value >> 32U));
#else
    return value;
#endif
}

std::uint64_t network_to_host64(std::uint64_t value) noexcept {
    return host_to_network64(value);
}

template <typename T> void append(std::vector<std::byte> *bytes, T value) {
    const auto *begin = reinterpret_cast<const std::byte *>(&value);
    bytes->insert(bytes->end(), begin, begin + sizeof(value));
}

template <typename T>
bool read(const std::vector<std::byte> &bytes, std::size_t *offset, T *value) {
    if (*offset > bytes.size() || bytes.size() - *offset < sizeof(T)) {
        return false;
    }
    std::memcpy(value, bytes.data() + *offset, sizeof(T));
    *offset += sizeof(T);
    return true;
}

bool empty_shape_except_payload(const DecodedControlRequest &request) noexcept {
    return request.value.length == 0 && request.value.access == 0 &&
           request.value.fd_indices.empty() && request.file_descriptors.empty();
}

bool empty_shape(const DecodedControlRequest &request) noexcept {
    return empty_shape_except_payload(request) && request.value.opaque.empty();
}

ControlServiceResult response_for(const DecodedControlRequest &request, int status = 0) {
    ControlServiceResult result;
    result.response.method = request.value.method;
    result.response.status = status;
    return result;
}

int validate_identity(std::uint64_t identity, ObjectType type) noexcept {
    const auto parts = decode_object_identity(identity);
    return parts.has_value() && parts->type == type ? 0 : EPROTO;
}

int call_identity(ControlClient &client, UgdrControlRequest request, ObjectType type,
                  std::uint64_t *identity) {
    if (identity == nullptr) {
        return EINVAL;
    }
    UgdrControlResponse response;
    const int call_status = client.call(std::move(request), &response);
    if (call_status != 0) {
        return call_status;
    }
    if (response.status != 0) {
        return response.status;
    }
    if (validate_identity(response.object_identity, type) != 0 || !response.opaque.empty() ||
        !response.fd_indices.empty()) {
        return EPROTO;
    }
    *identity = response.object_identity;
    return 0;
}

int call_destroy(ControlClient &client, UgdrControlRequest request) {
    UgdrControlResponse response;
    const int call_status = client.call(std::move(request), &response);
    if (call_status != 0) {
        return call_status;
    }
    if (response.status == 0 && (response.object_identity != 0 || !response.opaque.empty() ||
                                 !response.fd_indices.empty())) {
        return EPROTO;
    }
    return response.status;
}

}  // namespace

bool valid_qp_create_attributes(const QpCreateAttributes &attributes) noexcept {
    return attributes.send_cq_identity != 0 && attributes.recv_cq_identity != 0 &&
           attributes.max_send_wr != 0 && attributes.max_recv_wr != 0 &&
           attributes.max_send_sge != 0 && attributes.max_recv_sge != 0 &&
           attributes.qp_type == kQpTypeRc && attributes.sq_sig_all <= 1;
}

int encode_qp_create_attributes(const QpCreateAttributes &attributes,
                                std::vector<std::byte> *bytes) {
    if (bytes == nullptr) {
        return EINVAL;
    }
    std::vector<std::byte> encoded;
    encoded.reserve(kQpCreatePayloadSize);
    append(&encoded, htons(kQpPayloadVersion));
    append(&encoded, std::uint16_t{0});
    append(&encoded, host_to_network64(attributes.send_cq_identity));
    append(&encoded, host_to_network64(attributes.recv_cq_identity));
    append(&encoded, htonl(attributes.max_send_wr));
    append(&encoded, htonl(attributes.max_recv_wr));
    append(&encoded, htonl(attributes.max_send_sge));
    append(&encoded, htonl(attributes.max_recv_sge));
    append(&encoded, htonl(attributes.qp_type));
    append(&encoded, htonl(attributes.sq_sig_all));
    *bytes = std::move(encoded);
    return 0;
}

int decode_qp_create_attributes(const std::vector<std::byte> &bytes,
                                QpCreateAttributes *attributes) {
    if (attributes == nullptr || bytes.size() != kQpCreatePayloadSize) {
        return EPROTO;
    }
    std::size_t offset = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint64_t send_cq = 0;
    std::uint64_t recv_cq = 0;
    std::uint32_t max_send_wr = 0;
    std::uint32_t max_recv_wr = 0;
    std::uint32_t max_send_sge = 0;
    std::uint32_t max_recv_sge = 0;
    std::uint32_t qp_type = 0;
    std::uint32_t sq_sig_all = 0;
    if (!read(bytes, &offset, &version) || !read(bytes, &offset, &reserved) ||
        !read(bytes, &offset, &send_cq) || !read(bytes, &offset, &recv_cq) ||
        !read(bytes, &offset, &max_send_wr) || !read(bytes, &offset, &max_recv_wr) ||
        !read(bytes, &offset, &max_send_sge) || !read(bytes, &offset, &max_recv_sge) ||
        !read(bytes, &offset, &qp_type) || !read(bytes, &offset, &sq_sig_all)) {
        return EPROTO;
    }
    if (ntohs(version) != kQpPayloadVersion) {
        return EPROTONOSUPPORT;
    }
    if (reserved != 0) {
        return EPROTO;
    }
    QpCreateAttributes decoded;
    decoded.send_cq_identity = network_to_host64(send_cq);
    decoded.recv_cq_identity = network_to_host64(recv_cq);
    decoded.max_send_wr = ntohl(max_send_wr);
    decoded.max_recv_wr = ntohl(max_recv_wr);
    decoded.max_send_sge = ntohl(max_send_sge);
    decoded.max_recv_sge = ntohl(max_recv_sge);
    decoded.qp_type = ntohl(qp_type);
    decoded.sq_sig_all = ntohl(sq_sig_all);
    *attributes = decoded;
    return 0;
}

UgdrControlRequest make_create_qp_request(std::uint64_t pd_identity,
                                          const QpCreateAttributes &attributes) {
    UgdrControlRequest request;
    request.method = static_cast<std::uint32_t>(ControlMethod::create_qp);
    request.object_identity = pd_identity;
    (void)encode_qp_create_attributes(attributes, &request.opaque);
    return request;
}

UgdrControlRequest make_destroy_qp_request(std::uint64_t qp_identity) {
    UgdrControlRequest request;
    request.method = static_cast<std::uint32_t>(ControlMethod::destroy_qp);
    request.object_identity = qp_identity;
    return request;
}

QpService::QpService(gpu::CudaIpcMemoryBackend &memory_backend) : PdMrCqService(memory_backend) {
}

QpService::QpService(DeviceCatalog catalog, gpu::CudaIpcMemoryBackend &memory_backend)
    : PdMrCqService(std::move(catalog), memory_backend) {
}

ControlServiceResult QpService::handle(ipc::SessionId session_id, DecodedControlRequest request) {
    switch (static_cast<ControlMethod>(request.value.method)) {
    case ControlMethod::create_qp:
        return handle_create_qp(session_id, request);
    case ControlMethod::destroy_qp:
        return handle_destroy_qp(session_id, request);
    default:
        return PdMrCqService::handle(session_id, std::move(request));
    }
}

ControlServiceResult QpService::handle_create_qp(ipc::SessionId session_id,
                                                 DecodedControlRequest &request) {
    if (!empty_shape_except_payload(request)) {
        return response_for(request, EINVAL);
    }
    QpCreateAttributes attributes;
    const int decode_status = decode_qp_create_attributes(request.value.opaque, &attributes);
    if (decode_status != 0) {
        return response_for(request, decode_status);
    }
    if (!valid_qp_create_attributes(attributes)) {
        return response_for(request, EINVAL);
    }
    PdRecord *const pd = resolve_pd(session_id, request.value.object_identity);
    CqRecord *const send_cq = resolve_cq(session_id, attributes.send_cq_identity);
    CqRecord *const recv_cq = resolve_cq(session_id, attributes.recv_cq_identity);
    if (pd == nullptr || send_cq == nullptr || recv_cq == nullptr ||
        pd->context_identity != send_cq->context_identity ||
        pd->context_identity != recv_cq->context_identity) {
        return response_for(request, EINVAL);
    }

    QpRecord record;
    record.context_identity = pd->context_identity;
    record.pd_identity = request.value.object_identity;
    record.send_cq_identity = attributes.send_cq_identity;
    record.recv_cq_identity = attributes.recv_cq_identity;
    record.sq = {attributes.max_send_wr, attributes.max_send_sge};
    record.rq = {attributes.max_recv_wr, attributes.max_recv_sge};
    record.qp_type = attributes.qp_type;
    record.sq_sig_all = attributes.sq_sig_all;
    const auto identity = qps_.insert(session_id, std::move(record));
    if (!identity.has_value()) {
        return response_for(request, ENOSPC);
    }
    ++pd->qp_count;
    ++send_cq->qp_references;
    if (recv_cq != send_cq) {
        ++recv_cq->qp_references;
    }

    ControlServiceResult result = response_for(request);
    result.response.object_identity = *identity;
    return result;
}

ControlServiceResult QpService::handle_destroy_qp(ipc::SessionId session_id,
                                                  DecodedControlRequest &request) {
    if (!empty_shape(request)) {
        return response_for(request, EINVAL);
    }
    QpRecord *const qp = qps_.resolve(session_id, request.value.object_identity);
    if (qp == nullptr) {
        return response_for(request, EINVAL);
    }
    PdRecord *const pd = resolve_pd(session_id, qp->pd_identity);
    CqRecord *const send_cq = resolve_cq(session_id, qp->send_cq_identity);
    CqRecord *const recv_cq = resolve_cq(session_id, qp->recv_cq_identity);
    if (pd == nullptr || send_cq == nullptr || recv_cq == nullptr || pd->qp_count == 0 ||
        send_cq->qp_references == 0 || (recv_cq != send_cq && recv_cq->qp_references == 0)) {
        return response_for(request, EINVAL);
    }
    --pd->qp_count;
    --send_cq->qp_references;
    if (recv_cq != send_cq) {
        --recv_cq->qp_references;
    }
    return response_for(request, qps_.erase(session_id, request.value.object_identity));
}

void QpService::on_disconnect(ipc::SessionId session_id) noexcept {
    (void)qps_.erase_session(session_id);
    PdMrCqService::on_disconnect(session_id);
}

std::size_t QpService::qp_count() const noexcept {
    return qps_.size();
}

int client_create_qp(ControlClient &client, std::uint64_t pd_identity,
                     const QpCreateAttributes &attributes, std::uint64_t *qp_identity) {
    if (pd_identity == 0 || !valid_qp_create_attributes(attributes)) {
        return EINVAL;
    }
    return call_identity(client, make_create_qp_request(pd_identity, attributes), ObjectType::qp,
                         qp_identity);
}

int client_destroy_qp(ControlClient &client, std::uint64_t qp_identity) {
    return qp_identity == 0 ? EINVAL : call_destroy(client, make_destroy_qp_request(qp_identity));
}

}  // namespace ugdr::control

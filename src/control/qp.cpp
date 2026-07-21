#include "control/qp.hpp"

#include <arpa/inet.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace ugdr::control {
namespace {

constexpr std::size_t kQpCreatePayloadSize = 44;
constexpr std::size_t kQpQueryPayloadSize = 8;
constexpr std::size_t kQpModifyPayloadSize = 24;
constexpr std::size_t kQpConnectPayloadSize = 16;
constexpr std::size_t kQpSnapshotPayloadSize = 64;
constexpr std::size_t kQpConnInfoPayloadSize = 8;

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

int call_empty(ControlClient &client, UgdrControlRequest request) {
    return call_destroy(client, std::move(request));
}

void append_header(std::vector<std::byte> *bytes) {
    append(bytes, htons(kQpPayloadVersion));
    append(bytes, std::uint16_t{0});
}

int read_header(const std::vector<std::byte> &bytes, std::size_t expected_size,
                std::size_t *offset) {
    if (offset == nullptr || bytes.size() != expected_size) {
        return EPROTO;
    }
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    if (!read(bytes, offset, &version) || !read(bytes, offset, &reserved)) {
        return EPROTO;
    }
    if (ntohs(version) != kQpPayloadVersion) {
        return EPROTONOSUPPORT;
    }
    return reserved == 0 ? 0 : EPROTO;
}

std::vector<std::byte> encode_query_payload(std::uint32_t mask) {
    std::vector<std::byte> bytes;
    bytes.reserve(kQpQueryPayloadSize);
    append_header(&bytes);
    append(&bytes, htonl(mask));
    return bytes;
}

int decode_query_payload(const std::vector<std::byte> &bytes, std::uint32_t *mask) {
    std::size_t offset = 0;
    const int status = read_header(bytes, kQpQueryPayloadSize, &offset);
    std::uint32_t encoded = 0;
    if (status != 0 || mask == nullptr || !read(bytes, &offset, &encoded)) {
        return status != 0 ? status : EPROTO;
    }
    *mask = ntohl(encoded);
    return 0;
}

std::vector<std::byte> encode_modify_payload(const QpAttributes &attributes, std::uint32_t mask) {
    std::vector<std::byte> bytes;
    bytes.reserve(kQpModifyPayloadSize);
    append_header(&bytes);
    append(&bytes, htonl(mask));
    append(&bytes, htonl(attributes.state));
    append(&bytes, htonl(attributes.current_state));
    append(&bytes, htonl(attributes.access_flags));
    append(&bytes, attributes.timeout);
    append(&bytes, attributes.retry_count);
    append(&bytes, attributes.rnr_retry);
    append(&bytes, attributes.min_rnr_timer);
    return bytes;
}

int decode_modify_payload(const std::vector<std::byte> &bytes, QpAttributes *attributes,
                          std::uint32_t *mask) {
    std::size_t offset = 0;
    const int status = read_header(bytes, kQpModifyPayloadSize, &offset);
    std::uint32_t encoded_mask = 0, state = 0, current = 0, access = 0;
    QpAttributes decoded;
    if (status != 0 || attributes == nullptr || mask == nullptr ||
        !read(bytes, &offset, &encoded_mask) || !read(bytes, &offset, &state) ||
        !read(bytes, &offset, &current) || !read(bytes, &offset, &access) ||
        !read(bytes, &offset, &decoded.timeout) || !read(bytes, &offset, &decoded.retry_count) ||
        !read(bytes, &offset, &decoded.rnr_retry) ||
        !read(bytes, &offset, &decoded.min_rnr_timer)) {
        return status != 0 ? status : EPROTO;
    }
    decoded.state = ntohl(state);
    decoded.current_state = ntohl(current);
    decoded.access_flags = ntohl(access);
    *attributes = decoded;
    *mask = ntohl(encoded_mask);
    return 0;
}

std::vector<std::byte> encode_connect_payload(std::uint32_t qp_num, const QpAttributes &attributes,
                                              std::uint32_t mask) {
    std::vector<std::byte> bytes;
    bytes.reserve(kQpConnectPayloadSize);
    append_header(&bytes);
    append(&bytes, htonl(mask));
    append(&bytes, htonl(qp_num));
    append(&bytes, attributes.timeout);
    append(&bytes, attributes.retry_count);
    append(&bytes, attributes.rnr_retry);
    append(&bytes, attributes.min_rnr_timer);
    return bytes;
}

int decode_connect_payload(const std::vector<std::byte> &bytes, std::uint32_t *qp_num,
                           QpAttributes *attributes, std::uint32_t *mask) {
    std::size_t offset = 0;
    const int status = read_header(bytes, kQpConnectPayloadSize, &offset);
    std::uint32_t encoded_mask = 0, encoded_qp_num = 0;
    QpAttributes decoded;
    if (status != 0 || qp_num == nullptr || attributes == nullptr || mask == nullptr ||
        !read(bytes, &offset, &encoded_mask) || !read(bytes, &offset, &encoded_qp_num) ||
        !read(bytes, &offset, &decoded.timeout) || !read(bytes, &offset, &decoded.retry_count) ||
        !read(bytes, &offset, &decoded.rnr_retry) ||
        !read(bytes, &offset, &decoded.min_rnr_timer)) {
        return status != 0 ? status : EPROTO;
    }
    *qp_num = ntohl(encoded_qp_num);
    *attributes = decoded;
    *mask = ntohl(encoded_mask);
    return 0;
}

std::vector<std::byte> encode_conn_info(std::uint32_t qp_num) {
    std::vector<std::byte> bytes;
    bytes.reserve(kQpConnInfoPayloadSize);
    append_header(&bytes);
    append(&bytes, htonl(qp_num));
    return bytes;
}

int decode_conn_info(const std::vector<std::byte> &bytes, std::uint32_t *qp_num) {
    return decode_query_payload(bytes, qp_num);
}

std::vector<std::byte> encode_snapshot(const QpRecord &record) {
    std::vector<std::byte> bytes;
    bytes.reserve(kQpSnapshotPayloadSize);
    append_header(&bytes);
    append(&bytes, htonl(record.qp_num));
    append(&bytes, host_to_network64(record.send_cq_identity));
    append(&bytes, host_to_network64(record.recv_cq_identity));
    append(&bytes, htonl(record.sq.max_wr));
    append(&bytes, htonl(record.rq.max_wr));
    append(&bytes, htonl(record.sq.max_sge));
    append(&bytes, htonl(record.rq.max_sge));
    append(&bytes, htonl(record.qp_type));
    append(&bytes, htonl(record.sq_sig_all));
    append(&bytes, htonl(record.state));
    append(&bytes, htonl(record.state));
    append(&bytes, htonl(record.access_flags));
    append(&bytes, record.timeout);
    append(&bytes, record.retry_count);
    append(&bytes, record.rnr_retry);
    append(&bytes, record.min_rnr_timer);
    return bytes;
}

int decode_snapshot(const std::vector<std::byte> &bytes, QpSnapshot *snapshot) {
    std::size_t offset = 0;
    const int status = read_header(bytes, kQpSnapshotPayloadSize, &offset);
    if (status != 0 || snapshot == nullptr) {
        return status != 0 ? status : EPROTO;
    }
    std::uint32_t qp_num = 0, max_send_wr = 0, max_recv_wr = 0, max_send_sge = 0, max_recv_sge = 0,
                  qp_type = 0, sq_sig_all = 0, state = 0, current = 0, access = 0;
    std::uint64_t send_cq = 0, recv_cq = 0;
    QpSnapshot decoded;
    if (!read(bytes, &offset, &qp_num) || !read(bytes, &offset, &send_cq) ||
        !read(bytes, &offset, &recv_cq) || !read(bytes, &offset, &max_send_wr) ||
        !read(bytes, &offset, &max_recv_wr) || !read(bytes, &offset, &max_send_sge) ||
        !read(bytes, &offset, &max_recv_sge) || !read(bytes, &offset, &qp_type) ||
        !read(bytes, &offset, &sq_sig_all) || !read(bytes, &offset, &state) ||
        !read(bytes, &offset, &current) || !read(bytes, &offset, &access) ||
        !read(bytes, &offset, &decoded.attributes.timeout) ||
        !read(bytes, &offset, &decoded.attributes.retry_count) ||
        !read(bytes, &offset, &decoded.attributes.rnr_retry) ||
        !read(bytes, &offset, &decoded.attributes.min_rnr_timer)) {
        return EPROTO;
    }
    decoded.qp_num = ntohl(qp_num);
    decoded.creation = {network_to_host64(send_cq),
                        network_to_host64(recv_cq),
                        ntohl(max_send_wr),
                        ntohl(max_recv_wr),
                        ntohl(max_send_sge),
                        ntohl(max_recv_sge),
                        ntohl(qp_type),
                        ntohl(sq_sig_all)};
    decoded.attributes.state = ntohl(state);
    decoded.attributes.current_state = ntohl(current);
    decoded.attributes.access_flags = ntohl(access);
    *snapshot = decoded;
    return 0;
}

bool valid_query_mask(std::uint32_t mask) noexcept {
    constexpr std::uint32_t supported =
        kQpMaskState | kQpMaskCurrentState | kQpMaskAccess | kQpConnectMask;
    return (mask & ~supported) == 0;
}

bool valid_retry_attributes(const QpAttributes &attributes) noexcept {
    return attributes.retry_count <= 7 && attributes.rnr_retry <= 7 && attributes.timeout <= 31 &&
           attributes.min_rnr_timer <= 31;
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

UgdrControlRequest make_query_qp_request(std::uint64_t qp_identity, std::uint32_t attr_mask) {
    UgdrControlRequest request;
    request.method = static_cast<std::uint32_t>(ControlMethod::query_qp);
    request.object_identity = qp_identity;
    request.opaque = encode_query_payload(attr_mask);
    return request;
}

UgdrControlRequest make_modify_qp_request(std::uint64_t qp_identity, const QpAttributes &attributes,
                                          std::uint32_t attr_mask) {
    UgdrControlRequest request;
    request.method = static_cast<std::uint32_t>(ControlMethod::modify_qp);
    request.object_identity = qp_identity;
    request.opaque = encode_modify_payload(attributes, attr_mask);
    return request;
}

UgdrControlRequest make_query_qp_conn_info_request(std::uint64_t qp_identity) {
    UgdrControlRequest request;
    request.method = static_cast<std::uint32_t>(ControlMethod::query_qp_conn_info);
    request.object_identity = qp_identity;
    return request;
}

UgdrControlRequest make_connect_qp_request(std::uint64_t qp_identity, std::uint32_t remote_qp_num,
                                           const QpAttributes &attributes,
                                           std::uint32_t attr_mask) {
    UgdrControlRequest request;
    request.method = static_cast<std::uint32_t>(ControlMethod::connect_qp);
    request.object_identity = qp_identity;
    request.opaque = encode_connect_payload(remote_qp_num, attributes, attr_mask);
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
    case ControlMethod::query_qp:
        return handle_query_qp(session_id, request);
    case ControlMethod::modify_qp:
        return handle_modify_qp(session_id, request);
    case ControlMethod::query_qp_conn_info:
        return handle_query_qp_conn_info(session_id, request);
    case ControlMethod::connect_qp:
        return handle_connect_qp(session_id, request);
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
    if (next_qp_num_ == 0) {
        return response_for(request, ENOSPC);
    }
    record.qp_num = next_qp_num_++;
    const auto identity = qps_.insert(session_id, std::move(record));
    if (!identity.has_value()) {
        return response_for(request, ENOSPC);
    }
    if (!qp_num_index_.emplace(qps_.resolve(session_id, *identity)->qp_num, *identity).second) {
        (void)qps_.erase(session_id, *identity);
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
    const std::uint32_t qp_num = qp->qp_num;
    const int status = qps_.erase(session_id, request.value.object_identity);
    if (status == 0) {
        qp_num_index_.erase(qp_num);
    }
    return response_for(request, status);
}

ControlServiceResult QpService::handle_query_qp(ipc::SessionId session_id,
                                                DecodedControlRequest &request) {
    if (!empty_shape_except_payload(request)) {
        return response_for(request, EINVAL);
    }
    std::uint32_t mask = 0;
    const int decode_status = decode_query_payload(request.value.opaque, &mask);
    if (decode_status != 0) {
        return response_for(request, decode_status);
    }
    QpRecord *const qp = qps_.resolve(session_id, request.value.object_identity);
    if (qp == nullptr || !valid_query_mask(mask)) {
        return response_for(request, EINVAL);
    }
    ControlServiceResult result = response_for(request);
    result.response.opaque = encode_snapshot(*qp);
    return result;
}

ControlServiceResult QpService::handle_modify_qp(ipc::SessionId session_id,
                                                 DecodedControlRequest &request) {
    if (!empty_shape_except_payload(request)) {
        return response_for(request, EINVAL);
    }
    QpAttributes attributes;
    std::uint32_t mask = 0;
    const int decode_status = decode_modify_payload(request.value.opaque, &attributes, &mask);
    if (decode_status != 0) {
        return response_for(request, decode_status);
    }
    QpRecord *const qp = qps_.resolve(session_id, request.value.object_identity);
    if (qp == nullptr) {
        return response_for(request, EINVAL);
    }
    constexpr std::uint32_t supported_mask = kQpMaskState | kQpMaskCurrentState | kQpMaskAccess;
    if ((mask & kQpMaskState) == 0 || (mask & ~supported_mask) != 0) {
        return response_for(request, EINVAL);
    }
    if (attributes.state == kQpStateSqd || attributes.state == kQpStateSqe) {
        return response_for(request, EOPNOTSUPP);
    }
    if ((mask & kQpMaskCurrentState) != 0 && attributes.current_state != qp->state) {
        return response_for(request, EINVAL);
    }
    const std::uint32_t base_mask = mask & ~kQpMaskCurrentState;
    if (qp->state == kQpStateReset && attributes.state == kQpStateInit &&
        base_mask == (kQpMaskState | kQpMaskAccess) &&
        attributes.access_flags == kQpAccessRemoteWrite) {
        qp->access_flags = attributes.access_flags;
        qp->state = kQpStateInit;
        return response_for(request);
    }
    if ((qp->state == kQpStateReset || qp->state == kQpStateInit || qp->state == kQpStateRtr ||
         qp->state == kQpStateRts) &&
        attributes.state == kQpStateErr && base_mask == kQpMaskState) {
        qp->state = kQpStateErr;
        return response_for(request);
    }
    return response_for(request, EINVAL);
}

ControlServiceResult QpService::handle_query_qp_conn_info(ipc::SessionId session_id,
                                                          DecodedControlRequest &request) {
    if (!empty_shape(request)) {
        return response_for(request, EINVAL);
    }
    QpRecord *const qp = qps_.resolve(session_id, request.value.object_identity);
    if (qp == nullptr) {
        return response_for(request, EINVAL);
    }
    ControlServiceResult result = response_for(request);
    result.response.opaque = encode_conn_info(qp->qp_num);
    return result;
}

ControlServiceResult QpService::handle_connect_qp(ipc::SessionId session_id,
                                                  DecodedControlRequest &request) {
    if (!empty_shape_except_payload(request)) {
        return response_for(request, EINVAL);
    }
    std::uint32_t remote_qp_num = 0;
    std::uint32_t mask = 0;
    QpAttributes attributes;
    const int decode_status =
        decode_connect_payload(request.value.opaque, &remote_qp_num, &attributes, &mask);
    if (decode_status != 0) {
        return response_for(request, decode_status);
    }
    QpRecord *const local = qps_.resolve(session_id, request.value.object_identity);
    if (local == nullptr || remote_qp_num == 0 || mask != kQpConnectMask ||
        !valid_retry_attributes(attributes)) {
        return response_for(request, EINVAL);
    }
    if (local->peer_qp_num != 0 && local->peer_qp_num != remote_qp_num) {
        return response_for(request, EBUSY);
    }
    if (local->state != kQpStateInit) {
        return response_for(request, EINVAL);
    }
    const auto indexed = qp_num_index_.find(remote_qp_num);
    if (indexed == qp_num_index_.end()) {
        return response_for(request, ENOENT);
    }
    QpRecord *const remote = qps_.resolve_any(indexed->second);
    if (remote == nullptr) {
        return response_for(request, ENOENT);
    }
    if (remote->qp_type != kQpTypeRc ||
        (remote->state != kQpStateInit && remote->state != kQpStateRtr &&
         remote->state != kQpStateRts)) {
        return response_for(request, EINVAL);
    }
    local->peer_qp_num = remote_qp_num;
    local->peer_identity = indexed->second;
    local->timeout = attributes.timeout;
    local->retry_count = attributes.retry_count;
    local->rnr_retry = attributes.rnr_retry;
    local->min_rnr_timer = attributes.min_rnr_timer;
    local->state = kQpStateRts;
    return response_for(request);
}

void QpService::on_disconnect(ipc::SessionId session_id) noexcept {
    qps_.for_each_session(session_id, [this](std::uint64_t, const QpRecord &record) {
        qp_num_index_.erase(record.qp_num);
    });
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

int client_query_qp(ControlClient &client, std::uint64_t qp_identity, std::uint32_t attr_mask,
                    QpSnapshot *snapshot) {
    if (qp_identity == 0 || snapshot == nullptr || !valid_query_mask(attr_mask)) {
        return EINVAL;
    }
    UgdrControlResponse response;
    const int call_status = client.call(make_query_qp_request(qp_identity, attr_mask), &response);
    if (call_status != 0) {
        return call_status;
    }
    if (response.status != 0) {
        return response.status;
    }
    if (response.object_identity != 0 || !response.fd_indices.empty()) {
        return EPROTO;
    }
    QpSnapshot decoded;
    const int decode_status = decode_snapshot(response.opaque, &decoded);
    if (decode_status == 0) {
        *snapshot = decoded;
    }
    return decode_status;
}

int client_modify_qp(ControlClient &client, std::uint64_t qp_identity,
                     const QpAttributes &attributes, std::uint32_t attr_mask) {
    return qp_identity == 0
               ? EINVAL
               : call_empty(client, make_modify_qp_request(qp_identity, attributes, attr_mask));
}

int client_query_qp_conn_info(ControlClient &client, std::uint64_t qp_identity,
                              std::uint32_t *qp_num) {
    if (qp_identity == 0 || qp_num == nullptr) {
        return EINVAL;
    }
    UgdrControlResponse response;
    const int call_status = client.call(make_query_qp_conn_info_request(qp_identity), &response);
    if (call_status != 0) {
        return call_status;
    }
    if (response.status != 0) {
        return response.status;
    }
    if (response.object_identity != 0 || !response.fd_indices.empty()) {
        return EPROTO;
    }
    std::uint32_t decoded = 0;
    const int decode_status = decode_conn_info(response.opaque, &decoded);
    if (decode_status != 0 || decoded == 0) {
        return decode_status != 0 ? decode_status : EPROTO;
    }
    *qp_num = decoded;
    return 0;
}

int client_connect_qp(ControlClient &client, std::uint64_t qp_identity, std::uint32_t remote_qp_num,
                      const QpAttributes &attributes, std::uint32_t attr_mask) {
    return qp_identity == 0 || remote_qp_num == 0
               ? EINVAL
               : call_empty(client, make_connect_qp_request(qp_identity, remote_qp_num, attributes,
                                                            attr_mask));
}

}  // namespace ugdr::control

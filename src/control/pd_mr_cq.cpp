#include "control/pd_mr_cq.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace ugdr::control {
namespace {

constexpr std::size_t kMrRegistrationFixedSize = 48;
constexpr std::size_t kMrRegistrationResultSize = 28;

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

bool uuid_is_zero(const gpu::GpuUuid &uuid) noexcept {
    return std::all_of(uuid.begin(), uuid.end(), [](std::uint8_t value) { return value == 0; });
}

bool valid_memory(const gpu::ExportedCudaMemory &memory) noexcept {
    if (uuid_is_zero(memory.gpu_uuid) || memory.client_address == 0 || memory.length == 0 ||
        memory.allocation_size == 0 || memory.allocation_offset > memory.allocation_size ||
        memory.length > memory.allocation_size - memory.allocation_offset ||
        memory.client_address < memory.allocation_offset || memory.ipc_handle.empty() ||
        memory.ipc_handle.size() > gpu::kMaxCudaIpcHandleSize) {
        return false;
    }
    return true;
}

bool valid_access(std::uint32_t access) noexcept {
    constexpr std::uint32_t supported = kAccessLocalWrite | kAccessRemoteWrite;
    return (access & ~supported) == 0 &&
           ((access & kAccessRemoteWrite) == 0 || (access & kAccessLocalWrite) != 0);
}

bool empty_shape(const DecodedControlRequest &request) noexcept {
    return request.value.length == 0 && request.value.access == 0 && request.value.opaque.empty() &&
           request.value.fd_indices.empty() && request.file_descriptors.empty();
}

ControlServiceResult response_for(const DecodedControlRequest &request, int status = 0) {
    ControlServiceResult result;
    result.response.method = request.value.method;
    result.response.status = status;
    return result;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> allocate_keys(std::uint64_t *next_key) {
    if (next_key == nullptr || *next_key == 0 ||
        *next_key > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) - 1) {
        return std::nullopt;
    }
    const auto lkey = static_cast<std::uint32_t>(*next_key);
    const auto rkey = static_cast<std::uint32_t>(*next_key + 1);
    *next_key += 2;
    return std::pair{lkey, rkey};
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
    const int identity_status = validate_identity(response.object_identity, type);
    if (identity_status != 0 || !response.opaque.empty() || !response.fd_indices.empty()) {
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

UgdrControlRequest make_create_pd_request(std::uint64_t context_identity) {
    UgdrControlRequest request;
    request.method = static_cast<std::uint32_t>(ControlMethod::create_pd);
    request.object_identity = context_identity;
    return request;
}

UgdrControlRequest make_destroy_pd_request(std::uint64_t pd_identity) {
    UgdrControlRequest request;
    request.method = static_cast<std::uint32_t>(ControlMethod::destroy_pd);
    request.object_identity = pd_identity;
    return request;
}

UgdrControlRequest make_register_mr_request(std::uint64_t pd_identity,
                                            const gpu::ExportedCudaMemory &memory,
                                            std::uint32_t access) {
    UgdrControlRequest request;
    request.method = static_cast<std::uint32_t>(ControlMethod::register_mr);
    request.object_identity = pd_identity;
    request.length = memory.length;
    request.access = access;
    (void)encode_mr_registration(memory, &request.opaque);
    return request;
}

UgdrControlRequest make_deregister_mr_request(std::uint64_t mr_identity) {
    UgdrControlRequest request;
    request.method = static_cast<std::uint32_t>(ControlMethod::deregister_mr);
    request.object_identity = mr_identity;
    return request;
}

UgdrControlRequest make_create_cq_request(std::uint64_t context_identity, std::uint32_t cqe) {
    UgdrControlRequest request;
    request.method = static_cast<std::uint32_t>(ControlMethod::create_cq);
    request.object_identity = context_identity;
    request.length = cqe;
    return request;
}

UgdrControlRequest make_destroy_cq_request(std::uint64_t cq_identity) {
    UgdrControlRequest request;
    request.method = static_cast<std::uint32_t>(ControlMethod::destroy_cq);
    request.object_identity = cq_identity;
    return request;
}

int encode_mr_registration(const gpu::ExportedCudaMemory &memory, std::vector<std::byte> *bytes) {
    if (bytes == nullptr || !valid_memory(memory)) {
        return EINVAL;
    }
    std::vector<std::byte> encoded;
    encoded.reserve(kMrRegistrationFixedSize + memory.ipc_handle.size());
    append(&encoded, htons(kMrPayloadVersion));
    append(&encoded, std::uint16_t{0});
    const auto *uuid = reinterpret_cast<const std::byte *>(memory.gpu_uuid.data());
    encoded.insert(encoded.end(), uuid, uuid + memory.gpu_uuid.size());
    append(&encoded, host_to_network64(memory.client_address));
    append(&encoded, host_to_network64(memory.allocation_size));
    append(&encoded, host_to_network64(memory.allocation_offset));
    append(&encoded, htonl(static_cast<std::uint32_t>(memory.ipc_handle.size())));
    encoded.insert(encoded.end(), memory.ipc_handle.begin(), memory.ipc_handle.end());
    *bytes = std::move(encoded);
    return 0;
}

int decode_mr_registration(const std::vector<std::byte> &bytes, std::uint64_t length,
                           gpu::ExportedCudaMemory *memory) {
    if (memory == nullptr || bytes.size() < kMrRegistrationFixedSize) {
        return EPROTO;
    }
    std::size_t offset = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    if (!read(bytes, &offset, &version) || !read(bytes, &offset, &reserved)) {
        return EPROTO;
    }
    if (ntohs(version) != kMrPayloadVersion) {
        return EPROTONOSUPPORT;
    }
    if (reserved != 0) {
        return EPROTO;
    }
    gpu::ExportedCudaMemory decoded;
    std::memcpy(decoded.gpu_uuid.data(), bytes.data() + offset, decoded.gpu_uuid.size());
    offset += decoded.gpu_uuid.size();
    std::uint64_t client_address = 0;
    std::uint64_t allocation_size = 0;
    std::uint64_t allocation_offset = 0;
    std::uint32_t handle_size = 0;
    if (!read(bytes, &offset, &client_address) || !read(bytes, &offset, &allocation_size) ||
        !read(bytes, &offset, &allocation_offset) || !read(bytes, &offset, &handle_size)) {
        return EPROTO;
    }
    decoded.client_address = network_to_host64(client_address);
    decoded.allocation_size = network_to_host64(allocation_size);
    decoded.allocation_offset = network_to_host64(allocation_offset);
    decoded.length = length;
    handle_size = ntohl(handle_size);
    if (handle_size == 0 || handle_size > gpu::kMaxCudaIpcHandleSize ||
        handle_size != bytes.size() - offset) {
        return EPROTO;
    }
    decoded.ipc_handle.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());
    if (!valid_memory(decoded)) {
        return EINVAL;
    }
    *memory = std::move(decoded);
    return 0;
}

int encode_mr_registration_result(const MrRegistrationResult &result,
                                  std::vector<std::byte> *bytes) {
    if (bytes == nullptr || result.client_address == 0 || result.length == 0 || result.lkey == 0 ||
        result.rkey == 0) {
        return EINVAL;
    }
    std::vector<std::byte> encoded;
    encoded.reserve(kMrRegistrationResultSize);
    append(&encoded, htons(kMrPayloadVersion));
    append(&encoded, std::uint16_t{0});
    append(&encoded, host_to_network64(result.client_address));
    append(&encoded, host_to_network64(result.length));
    append(&encoded, htonl(result.lkey));
    append(&encoded, htonl(result.rkey));
    *bytes = std::move(encoded);
    return 0;
}

int decode_mr_registration_result(const std::vector<std::byte> &bytes,
                                  MrRegistrationResult *result) {
    if (result == nullptr || bytes.size() != kMrRegistrationResultSize) {
        return EPROTO;
    }
    std::size_t offset = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint64_t client_address = 0;
    std::uint64_t length = 0;
    std::uint32_t lkey = 0;
    std::uint32_t rkey = 0;
    if (!read(bytes, &offset, &version) || !read(bytes, &offset, &reserved) ||
        !read(bytes, &offset, &client_address) || !read(bytes, &offset, &length) ||
        !read(bytes, &offset, &lkey) || !read(bytes, &offset, &rkey) ||
        ntohs(version) != kMrPayloadVersion || reserved != 0) {
        return EPROTO;
    }
    MrRegistrationResult decoded;
    decoded.client_address = network_to_host64(client_address);
    decoded.length = network_to_host64(length);
    decoded.lkey = ntohl(lkey);
    decoded.rkey = ntohl(rkey);
    if (decoded.client_address == 0 || decoded.length == 0 || decoded.lkey == 0 ||
        decoded.rkey == 0) {
        return EPROTO;
    }
    *result = decoded;
    return 0;
}

PdMrCqService::PdMrCqService(gpu::CudaIpcMemoryBackend &memory_backend)
    : memory_backend_(memory_backend) {
}

PdMrCqService::PdMrCqService(DeviceCatalog catalog, gpu::CudaIpcMemoryBackend &memory_backend)
    : DeviceContextService(std::move(catalog)), memory_backend_(memory_backend) {
}

ControlServiceResult PdMrCqService::handle(ipc::SessionId session_id,
                                           DecodedControlRequest request) {
    switch (static_cast<ControlMethod>(request.value.method)) {
    case ControlMethod::create_pd:
        return handle_create_pd(session_id, request);
    case ControlMethod::destroy_pd:
        return handle_destroy_pd(session_id, request);
    case ControlMethod::register_mr:
        return handle_register_mr(session_id, request);
    case ControlMethod::deregister_mr:
        return handle_deregister_mr(session_id, request);
    case ControlMethod::create_cq:
        return handle_create_cq(session_id, request);
    case ControlMethod::destroy_cq:
        return handle_destroy_cq(session_id, request);
    default:
        return DeviceContextService::handle(session_id, std::move(request));
    }
}

ControlServiceResult PdMrCqService::handle_create_pd(ipc::SessionId session_id,
                                                     DecodedControlRequest &request) {
    if (!empty_shape(request)) {
        return response_for(request, EINVAL);
    }
    ContextRecord *const context = resolve_context(session_id, request.value.object_identity);
    if (context == nullptr) {
        return response_for(request, EINVAL);
    }
    const auto identity =
        pds_.insert(session_id, PdRecord{request.value.object_identity, {}, {}, {}, 0});
    if (!identity.has_value()) {
        return response_for(request, ENOSPC);
    }
    ++context->child_count;
    ControlServiceResult result = response_for(request);
    result.response.object_identity = *identity;
    return result;
}

ControlServiceResult PdMrCqService::handle_destroy_pd(ipc::SessionId session_id,
                                                      DecodedControlRequest &request) {
    if (!empty_shape(request)) {
        return response_for(request, EINVAL);
    }
    PdRecord *const pd = pds_.resolve(session_id, request.value.object_identity);
    if (pd == nullptr) {
        return response_for(request, EINVAL);
    }
    if (!pd->mr_identities.empty() || pd->qp_count != 0) {
        return response_for(request, EBUSY);
    }
    ContextRecord *const context = resolve_context(session_id, pd->context_identity);
    if (context == nullptr || context->child_count == 0) {
        return response_for(request, EINVAL);
    }
    const int status = pds_.erase(session_id, request.value.object_identity);
    if (status == 0) {
        --context->child_count;
    }
    return response_for(request, status);
}

ControlServiceResult PdMrCqService::handle_register_mr(ipc::SessionId session_id,
                                                       DecodedControlRequest &request) {
    if (!request.value.fd_indices.empty() || !request.file_descriptors.empty() ||
        !valid_access(request.value.access)) {
        return response_for(request, EINVAL);
    }
    PdRecord *const pd = pds_.resolve(session_id, request.value.object_identity);
    if (pd == nullptr) {
        return response_for(request, EINVAL);
    }
    gpu::ExportedCudaMemory memory;
    const int decode_status =
        decode_mr_registration(request.value.opaque, request.value.length, &memory);
    if (decode_status != 0) {
        return response_for(request, decode_status);
    }
    gpu::CudaIpcMapping mapping;
    const int open_status = memory_backend_.open(memory, &mapping);
    if (open_status != 0) {
        return response_for(request, open_status);
    }
    if (mapping.daemon_base_address == 0 || mapping.gpu_uuid != memory.gpu_uuid ||
        mapping.daemon_base_address >
            std::numeric_limits<std::uint64_t>::max() - memory.allocation_offset) {
        (void)memory_backend_.close(mapping);
        return response_for(request, EPROTO);
    }

    const auto keys = allocate_keys(&next_key_);
    if (!keys.has_value()) {
        (void)memory_backend_.close(mapping);
        return response_for(request, ENOSPC);
    }
    MrRegistrationResult accepted{memory.client_address, memory.length, keys->first, keys->second};
    std::vector<std::byte> encoded_result;
    const int encode_status = encode_mr_registration_result(accepted, &encoded_result);
    if (encode_status != 0) {
        (void)memory_backend_.close(mapping);
        return response_for(request, encode_status);
    }

    MrRecord record;
    record.context_identity = pd->context_identity;
    record.pd_identity = request.value.object_identity;
    record.gpu_uuid = memory.gpu_uuid;
    record.client_address = memory.client_address;
    record.allocation_size = memory.allocation_size;
    record.allocation_offset = memory.allocation_offset;
    record.daemon_address = mapping.daemon_base_address + memory.allocation_offset;
    record.length = memory.length;
    record.access = request.value.access;
    record.lkey = keys->first;
    record.rkey = keys->second;
    record.mapping = mapping;
    const auto identity = mrs_.insert(session_id, std::move(record));
    if (!identity.has_value()) {
        (void)memory_backend_.close(mapping);
        return response_for(request, ENOSPC);
    }

    bool relation_added = false;
    bool local_added = false;
    bool remote_added = false;
    try {
        relation_added = pd->mr_identities.insert(*identity).second;
        local_added = pd->local_key_index.emplace(keys->first, *identity).second;
        remote_added = pd->remote_key_index.emplace(keys->second, *identity).second;
    } catch (...) {
    }
    if (!relation_added || !local_added || !remote_added) {
        pd->mr_identities.erase(*identity);
        pd->local_key_index.erase(keys->first);
        pd->remote_key_index.erase(keys->second);
        (void)mrs_.erase(session_id, *identity);
        (void)memory_backend_.close(mapping);
        return response_for(request, ENOMEM);
    }

    ControlServiceResult result = response_for(request);
    result.response.object_identity = *identity;
    result.response.opaque = std::move(encoded_result);
    return result;
}

ControlServiceResult PdMrCqService::handle_deregister_mr(ipc::SessionId session_id,
                                                         DecodedControlRequest &request) {
    if (!empty_shape(request)) {
        return response_for(request, EINVAL);
    }
    MrRecord *const mr = mrs_.resolve(session_id, request.value.object_identity);
    if (mr == nullptr) {
        return response_for(request, EINVAL);
    }
    if (mr->work_request_references != 0) {
        return response_for(request, EBUSY);
    }
    const int close_status = memory_backend_.close(mr->mapping);
    if (close_status != 0) {
        return response_for(request, close_status);
    }
    PdRecord *const pd = pds_.resolve(session_id, mr->pd_identity);
    if (pd == nullptr) {
        return response_for(request, EINVAL);
    }
    pd->local_key_index.erase(mr->lkey);
    pd->remote_key_index.erase(mr->rkey);
    pd->mr_identities.erase(request.value.object_identity);
    return response_for(request, mrs_.erase(session_id, request.value.object_identity));
}

ControlServiceResult PdMrCqService::handle_create_cq(ipc::SessionId session_id,
                                                     DecodedControlRequest &request) {
    if (request.value.length == 0 ||
        request.value.length > std::numeric_limits<std::uint32_t>::max() ||
        request.value.access != 0 || !request.value.opaque.empty() ||
        !request.value.fd_indices.empty() || !request.file_descriptors.empty()) {
        return response_for(request, EINVAL);
    }
    ContextRecord *const context = resolve_context(session_id, request.value.object_identity);
    if (context == nullptr) {
        return response_for(request, EINVAL);
    }
    const auto identity =
        cqs_.insert(session_id, CqRecord{request.value.object_identity,
                                         static_cast<std::uint32_t>(request.value.length), 0});
    if (!identity.has_value()) {
        return response_for(request, ENOSPC);
    }
    ++context->child_count;
    ControlServiceResult result = response_for(request);
    result.response.object_identity = *identity;
    return result;
}

ControlServiceResult PdMrCqService::handle_destroy_cq(ipc::SessionId session_id,
                                                      DecodedControlRequest &request) {
    if (!empty_shape(request)) {
        return response_for(request, EINVAL);
    }
    CqRecord *const cq = cqs_.resolve(session_id, request.value.object_identity);
    if (cq == nullptr) {
        return response_for(request, EINVAL);
    }
    if (cq->qp_references != 0) {
        return response_for(request, EBUSY);
    }
    ContextRecord *const context = resolve_context(session_id, cq->context_identity);
    if (context == nullptr || context->child_count == 0) {
        return response_for(request, EINVAL);
    }
    const int status = cqs_.erase(session_id, request.value.object_identity);
    if (status == 0) {
        --context->child_count;
    }
    return response_for(request, status);
}

void PdMrCqService::on_disconnect(ipc::SessionId session_id) noexcept {
    mrs_.for_each_session(session_id, [this](std::uint64_t, MrRecord &mr) {
        (void)memory_backend_.close(mr.mapping);
    });
    (void)mrs_.erase_session(session_id);
    (void)cqs_.erase_session(session_id);
    (void)pds_.erase_session(session_id);
    DeviceContextService::on_disconnect(session_id);
}

int PdMrCqService::resolve_key(ipc::SessionId session_id, std::uint64_t pd_identity,
                               std::uint32_t key, std::uint64_t address, std::uint64_t length,
                               bool remote, std::uint64_t *daemon_address) const noexcept {
    if (key == 0 || length == 0 || daemon_address == nullptr) {
        return EINVAL;
    }
    const PdRecord *const pd = pds_.resolve(session_id, pd_identity);
    if (pd == nullptr) {
        return EINVAL;
    }
    const auto &index = remote ? pd->remote_key_index : pd->local_key_index;
    const auto found = index.find(key);
    if (found == index.end()) {
        return EINVAL;
    }
    const MrRecord *const mr = mrs_.resolve(session_id, found->second);
    if (mr == nullptr || mr->pd_identity != pd_identity) {
        return EINVAL;
    }
    if (remote && (mr->access & kAccessRemoteWrite) == 0) {
        return EACCES;
    }
    if (address < mr->client_address ||
        address > std::numeric_limits<std::uint64_t>::max() - length ||
        mr->client_address > std::numeric_limits<std::uint64_t>::max() - mr->length ||
        address + length > mr->client_address + mr->length) {
        return EINVAL;
    }
    const std::uint64_t offset = address - mr->client_address;
    if (mr->daemon_address > std::numeric_limits<std::uint64_t>::max() - offset) {
        return EINVAL;
    }
    *daemon_address = mr->daemon_address + offset;
    return 0;
}

int PdMrCqService::resolve_lkey(ipc::SessionId session_id, std::uint64_t pd_identity,
                                std::uint32_t lkey, std::uint64_t address, std::uint64_t length,
                                std::uint64_t *daemon_address) const noexcept {
    return resolve_key(session_id, pd_identity, lkey, address, length, false, daemon_address);
}

int PdMrCqService::resolve_rkey(ipc::SessionId session_id, std::uint64_t pd_identity,
                                std::uint32_t rkey, std::uint64_t address, std::uint64_t length,
                                std::uint64_t *daemon_address) const noexcept {
    return resolve_key(session_id, pd_identity, rkey, address, length, true, daemon_address);
}

std::size_t PdMrCqService::pd_count() const noexcept {
    return pds_.size();
}

std::size_t PdMrCqService::mr_count() const noexcept {
    return mrs_.size();
}

std::size_t PdMrCqService::cq_count() const noexcept {
    return cqs_.size();
}

PdRecord *PdMrCqService::resolve_pd(ipc::SessionId session_id, std::uint64_t identity) noexcept {
    return pds_.resolve(session_id, identity);
}

const PdRecord *PdMrCqService::resolve_pd(ipc::SessionId session_id,
                                          std::uint64_t identity) const noexcept {
    return pds_.resolve(session_id, identity);
}

CqRecord *PdMrCqService::resolve_cq(ipc::SessionId session_id, std::uint64_t identity) noexcept {
    return cqs_.resolve(session_id, identity);
}

const CqRecord *PdMrCqService::resolve_cq(ipc::SessionId session_id,
                                          std::uint64_t identity) const noexcept {
    return cqs_.resolve(session_id, identity);
}

int client_create_pd(ControlClient &client, std::uint64_t context_identity,
                     std::uint64_t *pd_identity) {
    return context_identity == 0 ? EINVAL
                                 : call_identity(client, make_create_pd_request(context_identity),
                                                 ObjectType::pd, pd_identity);
}

int client_destroy_pd(ControlClient &client, std::uint64_t pd_identity) {
    return pd_identity == 0 ? EINVAL : call_destroy(client, make_destroy_pd_request(pd_identity));
}

int client_register_mr(ControlClient &client, std::uint64_t pd_identity,
                       const gpu::ExportedCudaMemory &memory, std::uint32_t access,
                       std::uint64_t *mr_identity, MrRegistrationResult *result) {
    if (pd_identity == 0 || mr_identity == nullptr || result == nullptr || !valid_memory(memory) ||
        !valid_access(access)) {
        return EINVAL;
    }
    UgdrControlRequest request = make_register_mr_request(pd_identity, memory, access);
    if (request.opaque.empty()) {
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
    if (validate_identity(response.object_identity, ObjectType::mr) != 0 ||
        !response.fd_indices.empty()) {
        return EPROTO;
    }
    MrRegistrationResult decoded;
    const int decode_status = decode_mr_registration_result(response.opaque, &decoded);
    if (decode_status != 0 || decoded.client_address != memory.client_address ||
        decoded.length != memory.length) {
        return EPROTO;
    }
    *mr_identity = response.object_identity;
    *result = decoded;
    return 0;
}

int client_deregister_mr(ControlClient &client, std::uint64_t mr_identity) {
    return mr_identity == 0 ? EINVAL
                            : call_destroy(client, make_deregister_mr_request(mr_identity));
}

int client_create_cq(ControlClient &client, std::uint64_t context_identity, std::uint32_t cqe,
                     std::uint64_t *cq_identity) {
    return context_identity == 0 || cqe == 0
               ? EINVAL
               : call_identity(client, make_create_cq_request(context_identity, cqe),
                               ObjectType::cq, cq_identity);
}

int client_destroy_cq(ControlClient &client, std::uint64_t cq_identity) {
    return cq_identity == 0 ? EINVAL : call_destroy(client, make_destroy_cq_request(cq_identity));
}

}  // namespace ugdr::control

#include "control/device_context.hpp"

#include <arpa/inet.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

namespace ugdr::control {
namespace {

constexpr std::uint16_t kMaxDeviceNameLength = 255;

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

bool request_shape_is_empty(const DecodedControlRequest &request) {
    return request.value.length == 0 && request.value.access == 0 && request.value.opaque.empty() &&
           request.value.fd_indices.empty() && request.file_descriptors.empty();
}

ControlServiceResult error_response(const DecodedControlRequest &request, int status) {
    ControlServiceResult result;
    result.response.method = request.value.method;
    result.response.status = status;
    return result;
}

}  // namespace

UgdrControlRequest make_list_devices_request() {
    UgdrControlRequest request;
    request.method = static_cast<std::uint32_t>(ControlMethod::list_devices);
    return request;
}

UgdrControlRequest make_create_context_request(std::uint64_t device_identity) {
    UgdrControlRequest request;
    request.method = static_cast<std::uint32_t>(ControlMethod::create_context);
    request.object_identity = device_identity;
    return request;
}

UgdrControlRequest make_destroy_context_request(std::uint64_t context_identity) {
    UgdrControlRequest request;
    request.method = static_cast<std::uint32_t>(ControlMethod::destroy_context);
    request.object_identity = context_identity;
    return request;
}

int encode_device_list(const std::vector<DeviceDescriptor> &devices,
                       std::vector<std::byte> *bytes) {
    if (bytes == nullptr || devices.size() > std::numeric_limits<std::uint32_t>::max()) {
        return EINVAL;
    }
    std::size_t size = sizeof(std::uint32_t);
    for (const DeviceDescriptor &device : devices) {
        if (device.identity == 0 || device.name.size() > kMaxDeviceNameLength) {
            return EINVAL;
        }
        size += sizeof(std::uint64_t) + sizeof(std::uint16_t) + device.name.size();
        if (size > ipc::kMaxPayloadSize) {
            return EMSGSIZE;
        }
    }
    std::vector<std::byte> encoded;
    encoded.reserve(size);
    append(&encoded, htonl(static_cast<std::uint32_t>(devices.size())));
    for (const DeviceDescriptor &device : devices) {
        append(&encoded, host_to_network64(device.identity));
        append(&encoded, htons(static_cast<std::uint16_t>(device.name.size())));
        const auto *name = reinterpret_cast<const std::byte *>(device.name.data());
        encoded.insert(encoded.end(), name, name + device.name.size());
    }
    *bytes = std::move(encoded);
    return 0;
}

int decode_device_list(const std::vector<std::byte> &bytes,
                       std::vector<DeviceDescriptor> *devices) {
    if (devices == nullptr) {
        return EINVAL;
    }
    std::size_t offset = 0;
    std::uint32_t wire_count = 0;
    if (!read(bytes, &offset, &wire_count)) {
        return EPROTO;
    }
    const std::uint32_t count = ntohl(wire_count);
    constexpr std::size_t kMinimumDeviceSize = sizeof(std::uint64_t) + sizeof(std::uint16_t);
    if (count > (bytes.size() - offset) / kMinimumDeviceSize) {
        return EPROTO;
    }
    std::vector<DeviceDescriptor> decoded;
    decoded.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint64_t wire_identity = 0;
        std::uint16_t wire_name_length = 0;
        if (!read(bytes, &offset, &wire_identity) || !read(bytes, &offset, &wire_name_length)) {
            return EPROTO;
        }
        const std::size_t name_length = ntohs(wire_name_length);
        const std::uint64_t identity = network_to_host64(wire_identity);
        if (identity == 0 || name_length > bytes.size() - offset) {
            return EPROTO;
        }
        DeviceDescriptor device;
        device.identity = identity;
        device.name.assign(reinterpret_cast<const char *>(bytes.data() + offset), name_length);
        offset += name_length;
        decoded.push_back(std::move(device));
    }
    if (offset != bytes.size()) {
        return EPROTO;
    }
    *devices = std::move(decoded);
    return 0;
}

DeviceCatalog::DeviceCatalog() : devices_({{1, "ugdr0"}}) {
}

DeviceCatalog::DeviceCatalog(std::vector<DeviceDescriptor> devices) : devices_(std::move(devices)) {
}

const std::vector<DeviceDescriptor> &DeviceCatalog::devices() const noexcept {
    return devices_;
}

bool DeviceCatalog::contains(std::uint64_t identity) const noexcept {
    for (const DeviceDescriptor &device : devices_) {
        if (device.identity == identity) {
            return true;
        }
    }
    return false;
}

DeviceContextService::DeviceContextService() = default;

DeviceContextService::DeviceContextService(DeviceCatalog catalog) : catalog_(std::move(catalog)) {
}

ControlServiceResult DeviceContextService::handle(ipc::SessionId session_id,
                                                  DecodedControlRequest request) {
    const auto method = static_cast<ControlMethod>(request.value.method);
    if (!request_shape_is_empty(request)) {
        return error_response(request, EINVAL);
    }

    ControlServiceResult result;
    result.response.method = request.value.method;
    switch (method) {
    case ControlMethod::list_devices: {
        if (request.value.object_identity != 0) {
            result.response.status = EINVAL;
            break;
        }
        result.response.status = encode_device_list(catalog_.devices(), &result.response.opaque);
        break;
    }
    case ControlMethod::create_context: {
        if (!catalog_.contains(request.value.object_identity)) {
            result.response.status = EINVAL;
            break;
        }
        const auto identity =
            contexts_.insert(session_id, ContextRecord{request.value.object_identity, 0});
        if (!identity.has_value()) {
            result.response.status = ENOSPC;
            break;
        }
        result.response.object_identity = *identity;
        break;
    }
    case ControlMethod::destroy_context: {
        ContextRecord *context = contexts_.resolve(session_id, request.value.object_identity);
        if (context == nullptr) {
            result.response.status = EINVAL;
        } else if (context->child_count != 0) {
            result.response.status = EBUSY;
        } else {
            result.response.status = contexts_.erase(session_id, request.value.object_identity);
        }
        break;
    }
    default:
        result.response.status = EOPNOTSUPP;
        break;
    }
    return result;
}

void DeviceContextService::on_disconnect(ipc::SessionId session_id) noexcept {
    contexts_.erase_session(session_id);
}

std::size_t DeviceContextService::context_count() const noexcept {
    return contexts_.size();
}

ContextRecord *DeviceContextService::resolve_context(ipc::SessionId session_id,
                                                     std::uint64_t identity) noexcept {
    return contexts_.resolve(session_id, identity);
}

const ContextRecord *DeviceContextService::resolve_context(ipc::SessionId session_id,
                                                           std::uint64_t identity) const noexcept {
    return contexts_.resolve(session_id, identity);
}

class ControlClient::Impl {
  public:
    int call(UgdrControlRequest request, DecodedControlResponse *response) {
        ipc::IpcMessage encoded;
        const int encode_status = encode_request(request, {}, &encoded);
        if (encode_status != 0) {
            return -encode_status;
        }
        ipc::IpcMessage wire_response;
        const int call_status = client.call(request.method, std::move(encoded.payload),
                                            std::move(encoded.file_descriptors), &wire_response);
        if (call_status != 0) {
            client.close();
            return -call_status;
        }
        DecodedControlResponse decoded;
        const int decode_status = decode_response(std::move(wire_response), &decoded);
        if (decode_status != 0) {
            client.close();
            return -decode_status;
        }
        if (decoded.value.method != request.method || decoded.value.status < 0) {
            client.close();
            return EPROTO;
        }
        *response = std::move(decoded);
        return 0;
    }

    int call(UgdrControlRequest request, UgdrControlResponse *response) {
        DecodedControlResponse decoded;
        const int status = call(std::move(request), &decoded);
        if (status != 0) {
            return status;
        }
        if (!decoded.file_descriptors.empty()) {
            client.close();
            return EPROTO;
        }
        *response = std::move(decoded.value);
        return 0;
    }

    ipc::IpcClient client;
    std::uint64_t connection_epoch = 0;
};

ControlClient::ControlClient() : impl_(std::make_unique<Impl>()) {
}

ControlClient::~ControlClient() = default;

int ControlClient::connect(const std::string &socket_path) {
    const int status = impl_->client.connect(socket_path);
    if (status == 0) {
        ++impl_->connection_epoch;
        if (impl_->connection_epoch == 0) {
            ++impl_->connection_epoch;
        }
    }
    return status == 0 ? 0 : -status;
}

void ControlClient::close() noexcept {
    impl_->client.close();
}

bool ControlClient::connected() const noexcept {
    return impl_->client.connected();
}

std::uint64_t ControlClient::connection_epoch() const noexcept {
    return impl_->connection_epoch;
}

int ControlClient::list_devices(std::vector<DeviceDescriptor> *devices) {
    if (devices == nullptr) {
        return EINVAL;
    }
    UgdrControlResponse response;
    const int call_status = impl_->call(make_list_devices_request(), &response);
    if (call_status != 0) {
        return call_status;
    }
    if (response.status != 0) {
        return response.status;
    }
    return decode_device_list(response.opaque, devices);
}

int ControlClient::create_context(std::uint64_t device_identity, std::uint64_t *context_identity) {
    if (device_identity == 0 || context_identity == nullptr) {
        return EINVAL;
    }
    UgdrControlResponse response;
    const int call_status = impl_->call(make_create_context_request(device_identity), &response);
    if (call_status != 0) {
        return call_status;
    }
    if (response.status != 0) {
        return response.status;
    }
    const auto parts = decode_object_identity(response.object_identity);
    if (!parts.has_value() || parts->type != ObjectType::context) {
        return EPROTO;
    }
    *context_identity = response.object_identity;
    return 0;
}

int ControlClient::destroy_context(std::uint64_t context_identity) {
    if (context_identity == 0) {
        return EINVAL;
    }
    UgdrControlResponse response;
    const int call_status = impl_->call(make_destroy_context_request(context_identity), &response);
    return call_status != 0 ? call_status : response.status;
}

int ControlClient::call(UgdrControlRequest request, UgdrControlResponse *response) {
    if (response == nullptr) {
        return EINVAL;
    }
    return impl_->call(std::move(request), response);
}

int ControlClient::call(UgdrControlRequest request, DecodedControlResponse *response) {
    if (response == nullptr) {
        return EINVAL;
    }
    return impl_->call(std::move(request), response);
}

}  // namespace ugdr::control

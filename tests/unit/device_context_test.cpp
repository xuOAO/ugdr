#include "control/device_context.hpp"

#include <cerrno>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

ugdr::control::DecodedControlRequest decoded(ugdr::control::UgdrControlRequest request) {
    ugdr::control::DecodedControlRequest value;
    value.value = std::move(request);
    return value;
}

}  // namespace

int main() {
    using ugdr::control::ControlMethod;
    using ugdr::control::DeviceCatalog;
    using ugdr::control::DeviceContextService;
    using ugdr::control::DeviceDescriptor;

    const std::vector<DeviceDescriptor> expected{{1, "ugdr0"}, {7, "ugdr7"}, {9, "gpu-proxy"}};
    std::vector<std::byte> bytes;
    std::vector<DeviceDescriptor> actual;
    if (ugdr::control::encode_device_list(expected, &bytes) != 0 ||
        ugdr::control::decode_device_list(bytes, &actual) != 0 || actual != expected) {
        return 1;
    }
    bytes.pop_back();
    if (ugdr::control::decode_device_list(bytes, &actual) != EPROTO) {
        return 2;
    }

    DeviceContextService service{DeviceCatalog(expected)};
    auto listed = service.handle(11, decoded(ugdr::control::make_list_devices_request()));
    if (listed.response.status != 0 ||
        ugdr::control::decode_device_list(listed.response.opaque, &actual) != 0 ||
        actual != expected) {
        return 3;
    }

    auto created = service.handle(11, decoded(ugdr::control::make_create_context_request(7)));
    if (created.response.status != 0 || created.response.object_identity == 0 ||
        service.context_count() != 1) {
        return 4;
    }
    const std::uint64_t identity = created.response.object_identity;
    auto cross_session =
        service.handle(12, decoded(ugdr::control::make_destroy_context_request(identity)));
    if (cross_session.response.status != EINVAL || service.context_count() != 1) {
        return 5;
    }
    auto destroyed =
        service.handle(11, decoded(ugdr::control::make_destroy_context_request(identity)));
    auto repeated =
        service.handle(11, decoded(ugdr::control::make_destroy_context_request(identity)));
    if (destroyed.response.status != 0 || repeated.response.status != EINVAL ||
        service.context_count() != 0) {
        return 6;
    }

    created = service.handle(11, decoded(ugdr::control::make_create_context_request(1)));
    auto second = service.handle(12, decoded(ugdr::control::make_create_context_request(9)));
    if (created.response.status != 0 || second.response.status != 0 ||
        service.context_count() != 2) {
        return 7;
    }
    service.on_disconnect(11);
    if (service.context_count() != 1) {
        return 8;
    }
    service.on_disconnect(12);
    if (service.context_count() != 0) {
        return 9;
    }

    auto invalid_device =
        service.handle(11, decoded(ugdr::control::make_create_context_request(999)));
    ugdr::control::UgdrControlRequest unknown;
    unknown.method = 99;
    auto unsupported = service.handle(11, decoded(std::move(unknown)));
    return invalid_device.response.status == EINVAL && unsupported.response.status == EOPNOTSUPP &&
                   static_cast<std::uint32_t>(ControlMethod::list_devices) != 0
               ? 0
               : 10;
}

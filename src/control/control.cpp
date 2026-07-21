#include "control/control.hpp"

#include <cerrno>

#include <utility>

namespace ugdr {

int control_placeholder() noexcept {
    return 0;
}

namespace control {

void ControlService::on_disconnect(ipc::SessionId) noexcept {
}

ControlServiceResult UnsupportedControlService::handle(ipc::SessionId,
                                                       DecodedControlRequest request) {
    ControlServiceResult result;
    result.response.method = request.value.method;
    result.response.status = EOPNOTSUPP;
    result.response.object_identity = request.value.object_identity;
    return result;
}

ControlIpcHandler::ControlIpcHandler(ControlService &service) noexcept : service_(service) {
}

ipc::IpcMessage ControlIpcHandler::handle(ipc::SessionId session_id, ipc::IpcMessage &&request) {
    const std::uint32_t method = request.envelope.method;
    DecodedControlRequest decoded;
    const int decode_status = decode_request(std::move(request), &decoded);
    if (decode_status != 0) {
        ipc::IpcMessage response;
        response.envelope.method = method;
        response.envelope.status = -decode_status;
        return response;
    }

    ControlServiceResult service_result = service_.handle(session_id, std::move(decoded));
    service_result.response.method = method;
    ipc::IpcMessage response;
    const int encode_status = encode_response(
        service_result.response, std::move(service_result.file_descriptors), &response);
    if (encode_status != 0) {
        response = ipc::IpcMessage{};
        response.envelope.method = method;
        response.envelope.status = -encode_status;
    }
    return response;
}

void ControlIpcHandler::on_disconnect(ipc::SessionId session_id) noexcept {
    service_.on_disconnect(session_id);
}

}  // namespace control

}  // namespace ugdr

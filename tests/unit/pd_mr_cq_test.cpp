#include "control/pd_mr_cq.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace {

ugdr::control::DecodedControlRequest decoded(ugdr::control::UgdrControlRequest request) {
    ugdr::control::DecodedControlRequest value;
    value.value = std::move(request);
    return value;
}

class FakeCudaBackend final : public ugdr::gpu::CudaIpcMemoryBackend {
  public:
    int open(const ugdr::gpu::ExportedCudaMemory &memory,
             ugdr::gpu::CudaIpcMapping *mapping) override {
        ++open_calls;
        if (open_status != 0) {
            return open_status;
        }
        mapping->gpu_uuid = memory.gpu_uuid;
        mapping->daemon_base_address = next_base;
        next_base += UINT64_C(0x100000);
        ++live_mappings;
        return 0;
    }

    int close(const ugdr::gpu::CudaIpcMapping &) noexcept override {
        ++close_calls;
        if (close_status != 0) {
            return close_status;
        }
        --live_mappings;
        return 0;
    }

    int open_status = 0;
    int close_status = 0;
    int open_calls = 0;
    int close_calls = 0;
    int live_mappings = 0;
    std::uint64_t next_base = UINT64_C(0x80000000);
};

ugdr::gpu::ExportedCudaMemory sample_memory() {
    ugdr::gpu::ExportedCudaMemory memory;
    memory.gpu_uuid[0] = 7;
    memory.client_address = UINT64_C(0x10001000);
    memory.allocation_size = UINT64_C(0x4000);
    memory.allocation_offset = UINT64_C(0x1000);
    memory.length = UINT64_C(0x400);
    memory.ipc_handle.resize(64, std::byte{0x5a});
    return memory;
}

}  // namespace

int main() {
    using ugdr::control::MrRegistrationResult;
    using ugdr::control::PdMrCqService;

    const auto memory = sample_memory();
    std::vector<std::byte> bytes;
    ugdr::gpu::ExportedCudaMemory round_trip;
    if (ugdr::control::encode_mr_registration(memory, &bytes) != 0 ||
        ugdr::control::decode_mr_registration(bytes, memory.length, &round_trip) != 0 ||
        round_trip.gpu_uuid != memory.gpu_uuid ||
        round_trip.client_address != memory.client_address ||
        round_trip.allocation_size != memory.allocation_size ||
        round_trip.allocation_offset != memory.allocation_offset ||
        round_trip.length != memory.length || round_trip.ipc_handle != memory.ipc_handle) {
        return 1;
    }
    auto unsupported_version = bytes;
    unsupported_version[1] = std::byte{2};
    if (ugdr::control::decode_mr_registration(unsupported_version, memory.length, &round_trip) !=
        EPROTONOSUPPORT) {
        return 2;
    }
    auto invalid_uuid = memory;
    invalid_uuid.gpu_uuid.fill(0);
    if (ugdr::control::encode_mr_registration(invalid_uuid, &unsupported_version) != EINVAL) {
        return 2;
    }
    auto oversized_handle = memory;
    oversized_handle.ipc_handle.resize(ugdr::gpu::kMaxCudaIpcHandleSize + 1);
    if (ugdr::control::encode_mr_registration(oversized_handle, &unsupported_version) != EINVAL) {
        return 2;
    }
    bytes.pop_back();
    if (ugdr::control::decode_mr_registration(bytes, memory.length, &round_trip) != EPROTO) {
        return 2;
    }
    bytes.clear();
    MrRegistrationResult result{memory.client_address, memory.length, 11, 13};
    MrRegistrationResult result_round_trip;
    if (ugdr::control::encode_mr_registration_result(result, &bytes) != 0 ||
        ugdr::control::decode_mr_registration_result(bytes, &result_round_trip) != 0 ||
        result_round_trip.client_address != result.client_address ||
        result_round_trip.length != result.length || result_round_trip.lkey != result.lkey ||
        result_round_trip.rkey != result.rkey) {
        return 3;
    }
    bytes.pop_back();
    if (ugdr::control::decode_mr_registration_result(bytes, &result_round_trip) != EPROTO) {
        return 3;
    }

    FakeCudaBackend backend;
    PdMrCqService service(backend);
    constexpr ugdr::ipc::SessionId session = 11;
    auto context = service.handle(session, decoded(ugdr::control::make_create_context_request(1)));
    if (context.response.status != 0 || context.response.object_identity == 0) {
        return 4;
    }
    const auto context_identity = context.response.object_identity;
    auto pd =
        service.handle(session, decoded(ugdr::control::make_create_pd_request(context_identity)));
    auto second_pd =
        service.handle(session, decoded(ugdr::control::make_create_pd_request(context_identity)));
    auto cq = service.handle(session,
                             decoded(ugdr::control::make_create_cq_request(context_identity, 64)));
    if (pd.response.status != 0 || second_pd.response.status != 0 || cq.response.status != 0 ||
        service.pd_count() != 2 || service.cq_count() != 1) {
        return 5;
    }
    const auto pd_identity = pd.response.object_identity;
    const auto second_pd_identity = second_pd.response.object_identity;
    const auto cq_identity = cq.response.object_identity;
    auto busy_context = service.handle(
        session, decoded(ugdr::control::make_destroy_context_request(context_identity)));
    auto cross_session =
        service.handle(12, decoded(ugdr::control::make_destroy_pd_request(pd_identity)));
    if (busy_context.response.status != EBUSY || cross_session.response.status != EINVAL) {
        return 6;
    }

    auto malformed_request = ugdr::control::make_register_mr_request(
        pd_identity, memory, ugdr::control::kAccessLocalWrite);
    malformed_request.opaque.pop_back();
    auto malformed = service.handle(session, decoded(std::move(malformed_request)));
    if (malformed.response.status != EPROTO || backend.open_calls != 0 || service.mr_count() != 0) {
        return 7;
    }
    auto invalid_access =
        service.handle(session, decoded(ugdr::control::make_register_mr_request(
                                    pd_identity, memory, ugdr::control::kAccessRemoteWrite)));
    if (invalid_access.response.status != EINVAL || backend.open_calls != 0 ||
        service.mr_count() != 0) {
        return 7;
    }
    backend.open_status = EIO;
    auto failed_open =
        service.handle(session, decoded(ugdr::control::make_register_mr_request(
                                    pd_identity, memory, ugdr::control::kAccessLocalWrite)));
    backend.open_status = 0;
    if (failed_open.response.status != EIO || service.mr_count() != 0 ||
        backend.live_mappings != 0) {
        return 8;
    }

    auto registered = service.handle(
        session, decoded(ugdr::control::make_register_mr_request(
                     pd_identity, memory,
                     ugdr::control::kAccessLocalWrite | ugdr::control::kAccessRemoteWrite)));
    MrRegistrationResult accepted;
    if (registered.response.status != 0 || registered.response.object_identity == 0 ||
        ugdr::control::decode_mr_registration_result(registered.response.opaque, &accepted) != 0 ||
        accepted.lkey == 0 || accepted.rkey == 0 || accepted.lkey == accepted.rkey ||
        service.mr_count() != 1 || backend.live_mappings != 1) {
        return 9;
    }
    const auto mr_identity = registered.response.object_identity;
    std::uint64_t daemon_address = 0;
    if (service.resolve_lkey(session, pd_identity, accepted.lkey, memory.client_address + 32, 64,
                             &daemon_address) != 0 ||
        daemon_address != UINT64_C(0x80001020) ||
        service.resolve_rkey(session, pd_identity, accepted.rkey, memory.client_address + 64, 32,
                             &daemon_address) != 0 ||
        daemon_address != UINT64_C(0x80001040) ||
        service.resolve_lkey(session, second_pd_identity, accepted.lkey, memory.client_address, 1,
                             &daemon_address) != EINVAL ||
        service.resolve_lkey(session, pd_identity, accepted.lkey,
                             memory.client_address + memory.length, 1, &daemon_address) != EINVAL) {
        return 10;
    }
    auto busy_pd =
        service.handle(session, decoded(ugdr::control::make_destroy_pd_request(pd_identity)));
    if (busy_pd.response.status != EBUSY) {
        return 11;
    }

    backend.close_status = EIO;
    auto failed_close =
        service.handle(session, decoded(ugdr::control::make_deregister_mr_request(mr_identity)));
    if (failed_close.response.status != EIO || service.mr_count() != 1 ||
        backend.live_mappings != 1 ||
        service.resolve_lkey(session, pd_identity, accepted.lkey, memory.client_address, 1,
                             &daemon_address) != 0) {
        return 12;
    }
    backend.close_status = 0;
    auto deregistered =
        service.handle(session, decoded(ugdr::control::make_deregister_mr_request(mr_identity)));
    auto repeated =
        service.handle(session, decoded(ugdr::control::make_deregister_mr_request(mr_identity)));
    if (deregistered.response.status != 0 || repeated.response.status != EINVAL ||
        service.mr_count() != 0 || backend.live_mappings != 0 ||
        service.resolve_lkey(session, pd_identity, accepted.lkey, memory.client_address, 1,
                             &daemon_address) != EINVAL) {
        return 13;
    }

    auto local_only =
        service.handle(session, decoded(ugdr::control::make_register_mr_request(
                                    pd_identity, memory, ugdr::control::kAccessLocalWrite)));
    MrRegistrationResult local_keys;
    if (local_only.response.status != 0 ||
        ugdr::control::decode_mr_registration_result(local_only.response.opaque, &local_keys) !=
            0 ||
        local_keys.lkey <= accepted.lkey || local_keys.rkey <= accepted.rkey ||
        service.resolve_rkey(session, pd_identity, local_keys.rkey, memory.client_address, 1,
                             &daemon_address) != EACCES) {
        return 14;
    }
    if (service.handle(session, decoded(ugdr::control::make_deregister_mr_request(
                                    local_only.response.object_identity)))
                .response.status != 0 ||
        service.handle(session, decoded(ugdr::control::make_destroy_pd_request(pd_identity)))
                .response.status != 0 ||
        service.handle(session, decoded(ugdr::control::make_destroy_pd_request(second_pd_identity)))
                .response.status != 0 ||
        service.handle(session, decoded(ugdr::control::make_destroy_cq_request(cq_identity)))
                .response.status != 0 ||
        service.handle(session,
                       decoded(ugdr::control::make_destroy_context_request(context_identity)))
                .response.status != 0) {
        return 15;
    }

    context = service.handle(session, decoded(ugdr::control::make_create_context_request(1)));
    pd = service.handle(
        session, decoded(ugdr::control::make_create_pd_request(context.response.object_identity)));
    registered = service.handle(
        session, decoded(ugdr::control::make_register_mr_request(
                     pd.response.object_identity, memory, ugdr::control::kAccessLocalWrite)));
    cq = service.handle(session, decoded(ugdr::control::make_create_cq_request(
                                     context.response.object_identity, 8)));
    if (registered.response.status != 0 || cq.response.status != 0 || backend.live_mappings != 1) {
        return 16;
    }
    service.on_disconnect(session);
    return service.context_count() == 0 && service.pd_count() == 0 && service.mr_count() == 0 &&
                   service.cq_count() == 0 && backend.live_mappings == 0
               ? 0
               : 17;
}

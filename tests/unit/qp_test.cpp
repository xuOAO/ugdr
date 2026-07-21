#include "control/qp.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

ugdr::control::DecodedControlRequest decoded(ugdr::control::UgdrControlRequest request) {
    ugdr::control::DecodedControlRequest value;
    value.value = std::move(request);
    return value;
}

class UnusedCudaBackend final : public ugdr::gpu::CudaIpcMemoryBackend {
  public:
    int open(const ugdr::gpu::ExportedCudaMemory &, ugdr::gpu::CudaIpcMapping *) override {
        return EIO;
    }

    int close(const ugdr::gpu::CudaIpcMapping &) noexcept override {
        return EIO;
    }
};

ugdr::control::QpCreateAttributes attributes(std::uint64_t send_cq, std::uint64_t recv_cq) {
    return {send_cq, recv_cq, 17, 19, 3, 5, ugdr::control::kQpTypeRc, 1};
}

}  // namespace

int main() {
    using ugdr::control::QpCreateAttributes;
    using ugdr::control::QpService;

    const QpCreateAttributes encoded_attributes{UINT64_C(0x0102030405060708),
                                                UINT64_C(0x1112131415161718),
                                                17,
                                                19,
                                                3,
                                                5,
                                                ugdr::control::kQpTypeRc,
                                                1};
    std::vector<std::byte> bytes;
    QpCreateAttributes round_trip;
    if (ugdr::control::encode_qp_create_attributes(encoded_attributes, &bytes) != 0 ||
        ugdr::control::decode_qp_create_attributes(bytes, &round_trip) != 0 ||
        round_trip != encoded_attributes) {
        return 1;
    }
    auto unsupported_version = bytes;
    unsupported_version[1] = std::byte{2};
    if (ugdr::control::decode_qp_create_attributes(unsupported_version, &round_trip) !=
        EPROTONOSUPPORT) {
        return 2;
    }
    bytes.pop_back();
    if (ugdr::control::decode_qp_create_attributes(bytes, &round_trip) != EPROTO) {
        return 3;
    }

    UnusedCudaBackend backend;
    QpService service(backend);
    constexpr ugdr::ipc::SessionId session = 41;
    auto context = service.handle(session, decoded(ugdr::control::make_create_context_request(1)));
    auto other_context =
        service.handle(session, decoded(ugdr::control::make_create_context_request(1)));
    auto pd = service.handle(
        session, decoded(ugdr::control::make_create_pd_request(context.response.object_identity)));
    auto same_cq = service.handle(session, decoded(ugdr::control::make_create_cq_request(
                                               context.response.object_identity, 8)));
    auto send_cq = service.handle(session, decoded(ugdr::control::make_create_cq_request(
                                               context.response.object_identity, 9)));
    auto recv_cq = service.handle(session, decoded(ugdr::control::make_create_cq_request(
                                               context.response.object_identity, 10)));
    auto other_cq = service.handle(session, decoded(ugdr::control::make_create_cq_request(
                                                other_context.response.object_identity, 11)));
    if (context.response.status != 0 || other_context.response.status != 0 ||
        pd.response.status != 0 || same_cq.response.status != 0 || send_cq.response.status != 0 ||
        recv_cq.response.status != 0 || other_cq.response.status != 0) {
        return 4;
    }
    const std::uint64_t pd_identity = pd.response.object_identity;
    const std::uint64_t same_cq_identity = same_cq.response.object_identity;
    const std::uint64_t send_cq_identity = send_cq.response.object_identity;
    const std::uint64_t recv_cq_identity = recv_cq.response.object_identity;
    const std::uint64_t other_cq_identity = other_cq.response.object_identity;

    auto malformed_request = ugdr::control::make_create_qp_request(
        pd_identity, attributes(send_cq_identity, recv_cq_identity));
    malformed_request.opaque.pop_back();
    if (service.handle(session, decoded(std::move(malformed_request))).response.status != EPROTO ||
        service.qp_count() != 0) {
        return 5;
    }
    auto request_with_fd = decoded(ugdr::control::make_create_qp_request(
        pd_identity, attributes(send_cq_identity, recv_cq_identity)));
    const int descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return 6;
    }
    request_with_fd.value.fd_indices.push_back(0);
    request_with_fd.file_descriptors.emplace_back(descriptor);
    if (service.handle(session, std::move(request_with_fd)).response.status != EINVAL ||
        service.qp_count() != 0) {
        return 7;
    }

    QpCreateAttributes invalid = attributes(send_cq_identity, recv_cq_identity);
    invalid.max_send_wr = 0;
    if (service
            .handle(session, decoded(ugdr::control::make_create_qp_request(pd_identity, invalid)))
            .response.status != EINVAL) {
        return 8;
    }
    invalid = attributes(send_cq_identity, recv_cq_identity);
    invalid.max_recv_sge = 0;
    if (service
            .handle(session, decoded(ugdr::control::make_create_qp_request(pd_identity, invalid)))
            .response.status != EINVAL) {
        return 9;
    }
    invalid = attributes(send_cq_identity, recv_cq_identity);
    invalid.qp_type = 3;
    if (service
            .handle(session, decoded(ugdr::control::make_create_qp_request(pd_identity, invalid)))
            .response.status != EINVAL) {
        return 10;
    }
    invalid = attributes(send_cq_identity, recv_cq_identity);
    invalid.sq_sig_all = 2;
    if (service
            .handle(session, decoded(ugdr::control::make_create_qp_request(pd_identity, invalid)))
            .response.status != EINVAL) {
        return 11;
    }
    if (service.handle(session, decoded(ugdr::control::make_create_qp_request(
                                    pd_identity, attributes(send_cq_identity, other_cq_identity))))
                .response.status != EINVAL ||
        service.handle(42, decoded(ugdr::control::make_create_qp_request(
                               pd_identity, attributes(send_cq_identity, recv_cq_identity))))
                .response.status != EINVAL ||
        service.qp_count() != 0) {
        return 12;
    }

    auto same_qp =
        service.handle(session, decoded(ugdr::control::make_create_qp_request(
                                    pd_identity, attributes(same_cq_identity, same_cq_identity))));
    if (same_qp.response.status != 0 || service.qp_count() != 1 ||
        service.handle(session, decoded(ugdr::control::make_destroy_cq_request(same_cq_identity)))
                .response.status != EBUSY ||
        service.handle(session, decoded(ugdr::control::make_destroy_pd_request(pd_identity)))
                .response.status != EBUSY) {
        return 13;
    }
    const std::uint64_t same_qp_identity = same_qp.response.object_identity;
    if (service.handle(session, decoded(ugdr::control::make_destroy_qp_request(same_qp_identity)))
                .response.status != 0 ||
        service.handle(session, decoded(ugdr::control::make_destroy_qp_request(same_qp_identity)))
                .response.status != EINVAL ||
        service.handle(session, decoded(ugdr::control::make_destroy_cq_request(same_cq_identity)))
                .response.status != 0 ||
        service.qp_count() != 0) {
        return 14;
    }

    auto split_qp =
        service.handle(session, decoded(ugdr::control::make_create_qp_request(
                                    pd_identity, attributes(send_cq_identity, recv_cq_identity))));
    if (split_qp.response.status != 0 || service.qp_count() != 1 ||
        service.handle(session, decoded(ugdr::control::make_destroy_cq_request(send_cq_identity)))
                .response.status != EBUSY ||
        service.handle(session, decoded(ugdr::control::make_destroy_cq_request(recv_cq_identity)))
                .response.status != EBUSY) {
        return 15;
    }
    if (service.handle(session, decoded(ugdr::control::make_destroy_qp_request(
                                    split_qp.response.object_identity)))
                .response.status != 0 ||
        service.handle(session, decoded(ugdr::control::make_destroy_cq_request(send_cq_identity)))
                .response.status != 0 ||
        service.handle(session, decoded(ugdr::control::make_destroy_cq_request(recv_cq_identity)))
                .response.status != 0) {
        return 16;
    }

    auto final_cq = service.handle(session, decoded(ugdr::control::make_create_cq_request(
                                                context.response.object_identity, 12)));
    auto final_qp =
        service.handle(session, decoded(ugdr::control::make_create_qp_request(
                                    pd_identity, attributes(final_cq.response.object_identity,
                                                            final_cq.response.object_identity))));
    if (final_cq.response.status != 0 || final_qp.response.status != 0 || service.qp_count() != 1) {
        return 17;
    }
    service.on_disconnect(session);
    return service.qp_count() == 0 && service.cq_count() == 0 && service.pd_count() == 0 &&
                   service.context_count() == 0
               ? 0
               : 18;
}

#pragma once

#include "control/object_registry.hpp"
#include "control/pd_mr_cq.hpp"

#include <cstddef>
#include <cstdint>

namespace ugdr::control {

constexpr std::uint16_t kQpPayloadVersion = 1;
constexpr std::uint32_t kQpTypeRc = 2;
constexpr std::uint32_t kQpStateReset = 0;

struct QpCreateAttributes {
    std::uint64_t send_cq_identity = 0;
    std::uint64_t recv_cq_identity = 0;
    std::uint32_t max_send_wr = 0;
    std::uint32_t max_recv_wr = 0;
    std::uint32_t max_send_sge = 0;
    std::uint32_t max_recv_sge = 0;
    std::uint32_t qp_type = 0;
    std::uint32_t sq_sig_all = 0;

    bool operator==(const QpCreateAttributes &) const = default;
};

struct SqMetadata {
    std::uint32_t max_wr = 0;
    std::uint32_t max_sge = 0;
};

struct RqMetadata {
    std::uint32_t max_wr = 0;
    std::uint32_t max_sge = 0;
};

struct QpRecord {
    std::uint64_t context_identity = 0;
    std::uint64_t pd_identity = 0;
    std::uint64_t send_cq_identity = 0;
    std::uint64_t recv_cq_identity = 0;
    SqMetadata sq;
    RqMetadata rq;
    std::uint32_t qp_type = 0;
    std::uint32_t sq_sig_all = 0;
    std::uint32_t state = kQpStateReset;
};

bool valid_qp_create_attributes(const QpCreateAttributes &attributes) noexcept;
int encode_qp_create_attributes(const QpCreateAttributes &attributes,
                                std::vector<std::byte> *bytes);
int decode_qp_create_attributes(const std::vector<std::byte> &bytes,
                                QpCreateAttributes *attributes);
UgdrControlRequest make_create_qp_request(std::uint64_t pd_identity,
                                          const QpCreateAttributes &attributes);
UgdrControlRequest make_destroy_qp_request(std::uint64_t qp_identity);

class QpService final : public PdMrCqService {
  public:
    explicit QpService(gpu::CudaIpcMemoryBackend &memory_backend);
    QpService(DeviceCatalog catalog, gpu::CudaIpcMemoryBackend &memory_backend);

    ControlServiceResult handle(ipc::SessionId session_id, DecodedControlRequest request) override;
    void on_disconnect(ipc::SessionId session_id) noexcept override;

    [[nodiscard]] std::size_t qp_count() const noexcept;

  private:
    ControlServiceResult handle_create_qp(ipc::SessionId session_id,
                                          DecodedControlRequest &request);
    ControlServiceResult handle_destroy_qp(ipc::SessionId session_id,
                                           DecodedControlRequest &request);

    GenerationRegistry<QpRecord, ObjectType::qp> qps_;
};

int client_create_qp(ControlClient &client, std::uint64_t pd_identity,
                     const QpCreateAttributes &attributes, std::uint64_t *qp_identity);
int client_destroy_qp(ControlClient &client, std::uint64_t qp_identity);

}  // namespace ugdr::control

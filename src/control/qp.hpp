#pragma once

#include "control/object_registry.hpp"
#include "control/pd_mr_cq.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace ugdr::control {

constexpr std::uint16_t kQpPayloadVersion = 1;
constexpr std::uint32_t kQpTypeRc = 2;
constexpr std::uint32_t kQpStateReset = 0;
constexpr std::uint32_t kQpStateInit = 1;
constexpr std::uint32_t kQpStateRtr = 2;
constexpr std::uint32_t kQpStateRts = 3;
constexpr std::uint32_t kQpStateSqd = 4;
constexpr std::uint32_t kQpStateSqe = 5;
constexpr std::uint32_t kQpStateErr = 6;
constexpr std::uint32_t kQpAccessRemoteWrite = 1U << 1U;
constexpr std::uint32_t kQpMaskState = 1U << 0U;
constexpr std::uint32_t kQpMaskCurrentState = 1U << 1U;
constexpr std::uint32_t kQpMaskAccess = 1U << 3U;
constexpr std::uint32_t kQpMaskTimeout = 1U << 9U;
constexpr std::uint32_t kQpMaskRetryCount = 1U << 10U;
constexpr std::uint32_t kQpMaskRnrRetry = 1U << 11U;
constexpr std::uint32_t kQpMaskMinRnrTimer = 1U << 15U;
constexpr std::uint32_t kQpConnectMask =
    kQpMaskTimeout | kQpMaskRetryCount | kQpMaskRnrRetry | kQpMaskMinRnrTimer;

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

struct QpAttributes {
    std::uint32_t state = kQpStateReset;
    std::uint32_t current_state = kQpStateReset;
    std::uint32_t access_flags = 0;
    std::uint8_t timeout = 0;
    std::uint8_t retry_count = 0;
    std::uint8_t rnr_retry = 0;
    std::uint8_t min_rnr_timer = 0;

    bool operator==(const QpAttributes &) const = default;
};

struct QpSnapshot {
    QpCreateAttributes creation;
    QpAttributes attributes;
    std::uint32_t qp_num = 0;

    bool operator==(const QpSnapshot &) const = default;
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
    std::uint32_t access_flags = 0;
    std::uint32_t qp_num = 0;
    std::uint32_t peer_qp_num = 0;
    std::uint64_t peer_identity = 0;
    std::uint8_t timeout = 0;
    std::uint8_t retry_count = 0;
    std::uint8_t rnr_retry = 0;
    std::uint8_t min_rnr_timer = 0;
    queue::SharedRing send_queue;
    queue::SharedRing receive_queue;
};

bool valid_qp_create_attributes(const QpCreateAttributes &attributes) noexcept;
int encode_qp_create_attributes(const QpCreateAttributes &attributes,
                                std::vector<std::byte> *bytes);
int decode_qp_create_attributes(const std::vector<std::byte> &bytes,
                                QpCreateAttributes *attributes);
UgdrControlRequest make_create_qp_request(std::uint64_t pd_identity,
                                          const QpCreateAttributes &attributes);
UgdrControlRequest make_destroy_qp_request(std::uint64_t qp_identity);
UgdrControlRequest make_query_qp_request(std::uint64_t qp_identity, std::uint32_t attr_mask);
UgdrControlRequest make_modify_qp_request(std::uint64_t qp_identity, const QpAttributes &attributes,
                                          std::uint32_t attr_mask);
UgdrControlRequest make_query_qp_conn_info_request(std::uint64_t qp_identity);
UgdrControlRequest make_connect_qp_request(std::uint64_t qp_identity, std::uint32_t remote_qp_num,
                                           const QpAttributes &attributes, std::uint32_t attr_mask);

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
    ControlServiceResult handle_query_qp(ipc::SessionId session_id, DecodedControlRequest &request);
    ControlServiceResult handle_modify_qp(ipc::SessionId session_id,
                                          DecodedControlRequest &request);
    ControlServiceResult handle_query_qp_conn_info(ipc::SessionId session_id,
                                                   DecodedControlRequest &request);
    ControlServiceResult handle_connect_qp(ipc::SessionId session_id,
                                           DecodedControlRequest &request);

    GenerationRegistry<QpRecord, ObjectType::qp> qps_;
    std::unordered_map<std::uint32_t, std::uint64_t> qp_num_index_;
    std::uint32_t next_qp_num_ = 1;
};

int client_create_qp(ControlClient &client, std::uint64_t pd_identity,
                     const QpCreateAttributes &attributes, std::uint64_t *qp_identity,
                     queue::SharedRing *send_queue, queue::SharedRing *receive_queue);
int client_create_qp(ControlClient &client, std::uint64_t pd_identity,
                     const QpCreateAttributes &attributes, std::uint64_t *qp_identity);
int client_destroy_qp(ControlClient &client, std::uint64_t qp_identity);
int client_query_qp(ControlClient &client, std::uint64_t qp_identity, std::uint32_t attr_mask,
                    QpSnapshot *snapshot);
int client_modify_qp(ControlClient &client, std::uint64_t qp_identity,
                     const QpAttributes &attributes, std::uint32_t attr_mask);
int client_query_qp_conn_info(ControlClient &client, std::uint64_t qp_identity,
                              std::uint32_t *qp_num);
int client_connect_qp(ControlClient &client, std::uint64_t qp_identity, std::uint32_t remote_qp_num,
                      const QpAttributes &attributes, std::uint32_t attr_mask);

}  // namespace ugdr::control

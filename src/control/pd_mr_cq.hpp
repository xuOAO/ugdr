#pragma once

#include "control/device_context.hpp"
#include "control/object_registry.hpp"
#include "gpu/cuda_ipc_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace ugdr::control {

constexpr std::uint16_t kMrPayloadVersion = 1;
constexpr std::uint32_t kAccessLocalWrite = UINT32_C(1) << 0U;
constexpr std::uint32_t kAccessRemoteWrite = UINT32_C(1) << 1U;

struct MrRegistrationResult {
    std::uint64_t client_address = 0;
    std::uint64_t length = 0;
    std::uint32_t lkey = 0;
    std::uint32_t rkey = 0;
};

UgdrControlRequest make_create_pd_request(std::uint64_t context_identity);
UgdrControlRequest make_destroy_pd_request(std::uint64_t pd_identity);
UgdrControlRequest make_register_mr_request(std::uint64_t pd_identity,
                                            const gpu::ExportedCudaMemory &memory,
                                            std::uint32_t access);
UgdrControlRequest make_deregister_mr_request(std::uint64_t mr_identity);
UgdrControlRequest make_create_cq_request(std::uint64_t context_identity, std::uint32_t cqe);
UgdrControlRequest make_destroy_cq_request(std::uint64_t cq_identity);

int encode_mr_registration(const gpu::ExportedCudaMemory &memory, std::vector<std::byte> *bytes);
int decode_mr_registration(const std::vector<std::byte> &bytes, std::uint64_t length,
                           gpu::ExportedCudaMemory *memory);
int encode_mr_registration_result(const MrRegistrationResult &result,
                                  std::vector<std::byte> *bytes);
int decode_mr_registration_result(const std::vector<std::byte> &bytes,
                                  MrRegistrationResult *result);

struct PdRecord {
    std::uint64_t context_identity = 0;
    std::unordered_set<std::uint64_t> mr_identities;
    std::unordered_map<std::uint32_t, std::uint64_t> local_key_index;
    std::unordered_map<std::uint32_t, std::uint64_t> remote_key_index;
    std::size_t qp_count = 0;
};

struct MrRecord {
    std::uint64_t context_identity = 0;
    std::uint64_t pd_identity = 0;
    gpu::GpuUuid gpu_uuid{};
    std::uint64_t client_address = 0;
    std::uint64_t allocation_size = 0;
    std::uint64_t allocation_offset = 0;
    std::uint64_t daemon_address = 0;
    std::uint64_t length = 0;
    std::uint32_t access = 0;
    std::uint32_t lkey = 0;
    std::uint32_t rkey = 0;
    std::size_t work_request_references = 0;
    gpu::CudaIpcMapping mapping;
};

struct CqRecord {
    std::uint64_t context_identity = 0;
    std::uint32_t cqe = 0;
    std::size_t qp_references = 0;
};

class PdMrCqService final : public DeviceContextService {
  public:
    explicit PdMrCqService(gpu::CudaIpcMemoryBackend &memory_backend);
    PdMrCqService(DeviceCatalog catalog, gpu::CudaIpcMemoryBackend &memory_backend);

    ControlServiceResult handle(ipc::SessionId session_id, DecodedControlRequest request) override;
    void on_disconnect(ipc::SessionId session_id) noexcept override;

    int resolve_lkey(ipc::SessionId session_id, std::uint64_t pd_identity, std::uint32_t lkey,
                     std::uint64_t address, std::uint64_t length,
                     std::uint64_t *daemon_address) const noexcept;
    int resolve_rkey(ipc::SessionId session_id, std::uint64_t pd_identity, std::uint32_t rkey,
                     std::uint64_t address, std::uint64_t length,
                     std::uint64_t *daemon_address) const noexcept;

    [[nodiscard]] std::size_t pd_count() const noexcept;
    [[nodiscard]] std::size_t mr_count() const noexcept;
    [[nodiscard]] std::size_t cq_count() const noexcept;

  private:
    ControlServiceResult handle_create_pd(ipc::SessionId session_id,
                                          DecodedControlRequest &request);
    ControlServiceResult handle_destroy_pd(ipc::SessionId session_id,
                                           DecodedControlRequest &request);
    ControlServiceResult handle_register_mr(ipc::SessionId session_id,
                                            DecodedControlRequest &request);
    ControlServiceResult handle_deregister_mr(ipc::SessionId session_id,
                                              DecodedControlRequest &request);
    ControlServiceResult handle_create_cq(ipc::SessionId session_id,
                                          DecodedControlRequest &request);
    ControlServiceResult handle_destroy_cq(ipc::SessionId session_id,
                                           DecodedControlRequest &request);
    int resolve_key(ipc::SessionId session_id, std::uint64_t pd_identity, std::uint32_t key,
                    std::uint64_t address, std::uint64_t length, bool remote,
                    std::uint64_t *daemon_address) const noexcept;

    gpu::CudaIpcMemoryBackend &memory_backend_;
    GenerationRegistry<PdRecord, ObjectType::pd> pds_;
    GenerationRegistry<MrRecord, ObjectType::mr> mrs_;
    GenerationRegistry<CqRecord, ObjectType::cq> cqs_;
    std::uint64_t next_key_ = 1;
};

int client_create_pd(ControlClient &client, std::uint64_t context_identity,
                     std::uint64_t *pd_identity);
int client_destroy_pd(ControlClient &client, std::uint64_t pd_identity);
int client_register_mr(ControlClient &client, std::uint64_t pd_identity,
                       const gpu::ExportedCudaMemory &memory, std::uint32_t access,
                       std::uint64_t *mr_identity, MrRegistrationResult *result);
int client_deregister_mr(ControlClient &client, std::uint64_t mr_identity);
int client_create_cq(ControlClient &client, std::uint64_t context_identity, std::uint32_t cqe,
                     std::uint64_t *cq_identity);
int client_destroy_cq(ControlClient &client, std::uint64_t cq_identity);

}  // namespace ugdr::control

#pragma once

#include "control/qp.hpp"
#include "gpu/cuda_ipc_memory.hpp"
#include "gpu/persistent_copy_backend.hpp"
#include "worker/local_transport.hpp"
#include "worker/worker.hpp"

#include <cstdint>
#include <memory>

namespace ugdr::daemon {

class DataService final : public control::ControlService {
  public:
    explicit DataService(std::uint64_t max_batch_delay_nanoseconds = 50'000);

    control::ControlServiceResult handle(ipc::SessionId session_id,
                                         control::DecodedControlRequest request) override;
    void on_disconnect(ipc::SessionId session_id) noexcept override;

    void progress_once();
    void request_stop() noexcept;

    [[nodiscard]] int initialization_status() const noexcept;
    [[nodiscard]] int status() const noexcept;
    [[nodiscard]] bool workers_ready() const noexcept;
    [[nodiscard]] bool stopped() const noexcept;

  private:
    void reconcile_connected_qps();
    int start_backend();
    int stop_backend(bool preserve_workers) noexcept;
    void progress_backend_shutdown();
    void record_status(int status) noexcept;

    static constexpr std::size_t kRequestCapacity = 512;
    static constexpr std::size_t kResponseCapacity = 64;
    static constexpr std::size_t kBackendCapacity = 1024;

    gpu::RuntimeCudaIpcMemoryBackend memory_backend_;
    control::QpService service_;
    std::unique_ptr<worker::LocalTransport> transport_;
    gpu::PersistentCudaCopyBackend backend_;
    std::unique_ptr<worker::LoopWorker> requester_;
    std::unique_ptr<worker::LoopWorker> responder_;
    ipc::SessionId stage_buffer_session_ = 0;
    std::uint64_t stage_buffer_base_ = 0;
    std::uint64_t stage_buffer_bytes_ = 0;
    std::uint64_t max_batch_delay_nanoseconds_ = 0;
    int status_ = 0;
    bool stop_requested_ = false;
};

}  // namespace ugdr::daemon

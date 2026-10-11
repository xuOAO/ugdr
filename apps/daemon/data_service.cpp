#include "data_service.hpp"

#include "control/pd_mr_cq.hpp"

#include <array>
#include <cerrno>
#include <utility>

namespace ugdr::daemon {

DataService::DataService(std::uint64_t max_batch_delay_nanoseconds)
    : service_(memory_backend_), max_batch_delay_nanoseconds_(max_batch_delay_nanoseconds) {
}

control::ControlServiceResult DataService::handle(ipc::SessionId session_id,
                                                  control::DecodedControlRequest request) {
    const auto method = static_cast<control::ControlMethod>(request.value.method);
    gpu::ExportedCudaMemory source_registration;
    const bool registering_stage_buffer =
        method == control::ControlMethod::register_mr &&
        request.value.access == control::kAccessLocalWrite &&
        control::decode_mr_registration(request.value.opaque, request.value.length,
                                        &source_registration) == 0;
    const std::uint64_t registration_pd = request.value.object_identity;
    if ((method == control::ControlMethod::deregister_mr ||
         method == control::ControlMethod::destroy_qp) &&
        backend_.state() != gpu::PersistentCudaCopyBackendState::stopped) {
        const int stop_status = stop_backend(true);
        if (stop_status != 0) {
            record_status(stop_status);
            control::ControlServiceResult failure;
            failure.response.method = request.value.method;
            failure.response.status = stop_status;
            return failure;
        }
    }
    auto result = service_.handle(session_id, std::move(request));
    if (result.response.status == 0 && registering_stage_buffer) {
        control::MrRegistrationResult registration_result;
        std::uint64_t daemon_address = 0;
        if (control::decode_mr_registration_result(result.response.opaque, &registration_result) !=
                0 ||
            service_.resolve_lkey(session_id, registration_pd, registration_result.lkey,
                                  source_registration.client_address, source_registration.length,
                                  &daemon_address) != 0) {
            record_status(EPROTO);
        } else {
            stage_buffer_session_ = session_id;
            stage_buffer_base_ = daemon_address;
            stage_buffer_bytes_ = source_registration.length;
        }
    }
    if (result.response.status == 0 && method == control::ControlMethod::register_mr &&
        workers_ready() && backend_.state() == gpu::PersistentCudaCopyBackendState::stopped) {
        record_status(start_backend());
    }
    return result;
}

void DataService::on_disconnect(ipc::SessionId session_id) noexcept {
    const int stop_status = stop_backend(false);
    if (stop_status != 0) {
        record_status(stop_status);
    }
    service_.on_disconnect(session_id);
}

void DataService::progress_once() {
    if (status_ != 0) {
        return;
    }
    if (stop_requested_ || service_.qp_count() == 0) {
        progress_backend_shutdown();
        return;
    }
    reconcile_connected_qps();
    if (!workers_ready()) {
        return;
    }
    (void)requester_->progress_once();
    (void)responder_->progress_once();
    (void)responder_->progress_once();
    (void)requester_->progress_once();
}

void DataService::request_stop() noexcept {
    stop_requested_ = true;
}

int DataService::initialization_status() const noexcept {
    return memory_backend_.initialization_status();
}

int DataService::status() const noexcept {
    return status_;
}

bool DataService::workers_ready() const noexcept {
    return requester_ != nullptr && responder_ != nullptr;
}

bool DataService::stopped() const noexcept {
    return backend_.state() == gpu::PersistentCudaCopyBackendState::stopped;
}

void DataService::reconcile_connected_qps() {
    if (workers_ready() || stop_requested_ || stage_buffer_session_ == 0 ||
        stage_buffer_base_ == 0 || stage_buffer_bytes_ == 0) {
        return;
    }
    std::uint32_t requester_qp_num = 0;
    std::uint32_t responder_qp_num = 0;
    for (const std::uint32_t qp_num : service_.worker_qp_nums()) {
        control::WorkerQpView requester_view;
        if (service_.worker_qp_view(qp_num, &requester_view) != 0 ||
            requester_view.session_id != stage_buffer_session_ || requester_view.peer_qp_num == 0) {
            continue;
        }
        control::WorkerQpView responder_view;
        if (service_.worker_qp_view(requester_view.peer_qp_num, &responder_view) != 0 ||
            responder_view.peer_qp_num != requester_view.qp_num) {
            continue;
        }
        requester_qp_num = requester_view.qp_num;
        responder_qp_num = responder_view.qp_num;
        break;
    }
    if (requester_qp_num == 0 || responder_qp_num == 0) {
        return;
    }

    const int start_status = start_backend();
    if (start_status != 0) {
        record_status(start_status);
        return;
    }
    transport_ = std::make_unique<worker::LocalTransport>(kRequestCapacity, kResponseCapacity);
    requester_ = std::make_unique<worker::LoopWorker>(service_, requester_qp_num, *transport_,
                                                      backend_, worker::LoopWorkerRole::requester);
    responder_ = std::make_unique<worker::LoopWorker>(service_, responder_qp_num, *transport_,
                                                      backend_, worker::LoopWorkerRole::responder);
}

int DataService::start_backend() {
    if (backend_.state() != gpu::PersistentCudaCopyBackendState::stopped) {
        return 0;
    }
    gpu::PersistentCudaCopyBackendConfig config;
    config.device_ordinal = 0;
    config.stage_buffer_base = stage_buffer_base_;
    config.stage_buffer_bytes = stage_buffer_bytes_;
    config.queue_capacity = kBackendCapacity;
    config.host_batch = gpu::kPersistentCudaCopyBackendHostBatch;
    config.max_batch_delay_nanoseconds = max_batch_delay_nanoseconds_;
    return backend_.start(config);
}

int DataService::stop_backend(bool preserve_workers) noexcept {
    if (backend_.state() == gpu::PersistentCudaCopyBackendState::accepting) {
        const int status = backend_.request_stop();
        if (status != 0) {
            return status;
        }
    }
    for (std::size_t iteration = 0;
         backend_.state() == gpu::PersistentCudaCopyBackendState::draining && iteration < 1'000'000;
         ++iteration) {
        if (workers_ready()) {
            (void)requester_->progress_once();
            (void)responder_->progress_once();
            (void)responder_->progress_once();
            (void)requester_->progress_once();
        } else {
            std::array<worker::BackendCompletion, gpu::kPersistentCudaCopyBackendHostBatch>
                completions{};
            (void)backend_.try_pop_completion_batch(completions.data(), completions.size());
        }
        const int status = backend_.wait();
        if (status == 0) {
            break;
        }
        if (status != EAGAIN) {
            return status;
        }
    }
    if (backend_.state() != gpu::PersistentCudaCopyBackendState::stopped) {
        return ETIMEDOUT;
    }
    if (!preserve_workers) {
        requester_.reset();
        responder_.reset();
        transport_.reset();
    }
    return 0;
}

void DataService::progress_backend_shutdown() {
    record_status(stop_backend(false));
}

void DataService::record_status(int status) noexcept {
    if (status_ == 0 && status != 0) {
        status_ = status;
    }
}

}  // namespace ugdr::daemon

#include "api/wr_posting.hpp"
#include "control/qp.hpp"
#include "queue/completion_queue.hpp"
#include "support/loop_worker_fixture.hpp"
#include "support/mock_worker_fixture.hpp"
#include "worker/worker.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

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
        mapping->gpu_uuid = memory.gpu_uuid;
        mapping->daemon_base_address = next_base;
        next_base += UINT64_C(0x100000);
        return 0;
    }

    int close(const ugdr::gpu::CudaIpcMapping &) noexcept override {
        return 0;
    }

    std::uint64_t next_base = UINT64_C(0x80000000);
};

struct Endpoint {
    ugdr::ipc::SessionId session = 0;
    std::uint64_t context_identity = 0;
    std::uint64_t pd_identity = 0;
    std::uint64_t cq_identity = 0;
    std::uint64_t qp_identity = 0;
    std::uint64_t mr_identity = 0;
    ugdr::gpu::ExportedCudaMemory memory;
    ugdr::control::MrRegistrationResult registration;
    std::uint32_t qp_num = 0;
};

bool make_endpoint(ugdr::control::QpService &service, ugdr::ipc::SessionId session,
                   std::uint64_t client_address, Endpoint *endpoint, bool sq_sig_all = false) {
    endpoint->session = session;
    endpoint->memory.gpu_uuid[0] = 7;
    endpoint->memory.client_address = client_address;
    endpoint->memory.allocation_size = 4096;
    endpoint->memory.length = 4096;
    endpoint->memory.ipc_handle.resize(64, std::byte{0x5a});

    auto context = service.handle(session, decoded(ugdr::control::make_create_context_request(1)));
    auto pd = service.handle(
        session, decoded(ugdr::control::make_create_pd_request(context.response.object_identity)));
    auto cq = service.handle(session, decoded(ugdr::control::make_create_cq_request(
                                          context.response.object_identity, 8)));
    auto mr = service.handle(
        session, decoded(ugdr::control::make_register_mr_request(
                     pd.response.object_identity, endpoint->memory,
                     ugdr::control::kAccessLocalWrite | ugdr::control::kAccessRemoteWrite)));
    ugdr::control::QpCreateAttributes attributes;
    attributes.send_cq_identity = cq.response.object_identity;
    attributes.recv_cq_identity = cq.response.object_identity;
    attributes.max_send_wr = 8;
    attributes.max_recv_wr = 8;
    attributes.max_send_sge = 4;
    attributes.max_recv_sge = 4;
    attributes.qp_type = ugdr::control::kQpTypeRc;
    attributes.sq_sig_all = sq_sig_all ? 1 : 0;
    auto qp = service.handle(session, decoded(ugdr::control::make_create_qp_request(
                                          pd.response.object_identity, attributes)));
    if (context.response.status != 0 || pd.response.status != 0 || cq.response.status != 0 ||
        mr.response.status != 0 || qp.response.status != 0 ||
        ugdr::control::decode_mr_registration_result(mr.response.opaque, &endpoint->registration) !=
            0) {
        return false;
    }
    endpoint->context_identity = context.response.object_identity;
    endpoint->pd_identity = pd.response.object_identity;
    endpoint->cq_identity = cq.response.object_identity;
    endpoint->mr_identity = mr.response.object_identity;
    endpoint->qp_identity = qp.response.object_identity;
    ugdr::control::WorkerQpView view;
    if (service.worker_qp_view(0, &view) != EINVAL ||
        service.worker_qp_view(UINT32_MAX, &view) != ENOENT) {
        return false;
    }
    for (std::uint32_t qp_num = 1; qp_num != UINT32_MAX; ++qp_num) {
        if (service.worker_qp_view(qp_num, &view) == 0 && view.session_id == session) {
            endpoint->qp_num = qp_num;
            return true;
        }
    }
    return false;
}

bool connect_endpoints(ugdr::control::QpService &service, const Endpoint &first,
                       const Endpoint &second) {
    ugdr::control::QpAttributes init;
    init.state = ugdr::control::kQpStateInit;
    init.current_state = ugdr::control::kQpStateReset;
    init.access_flags = ugdr::control::kQpAccessRemoteWrite;
    constexpr std::uint32_t init_mask = ugdr::control::kQpMaskState |
                                        ugdr::control::kQpMaskCurrentState |
                                        ugdr::control::kQpMaskAccess;
    if (service.handle(first.session, decoded(ugdr::control::make_modify_qp_request(
                                          first.qp_identity, init, init_mask)))
                .response.status != 0 ||
        service.handle(second.session, decoded(ugdr::control::make_modify_qp_request(
                                           second.qp_identity, init, init_mask)))
                .response.status != 0) {
        return false;
    }
    ugdr::control::QpAttributes retry;
    retry.timeout = 1;
    retry.retry_count = 1;
    retry.rnr_retry = 1;
    retry.min_rnr_timer = 1;
    return service.handle(first.session, decoded(ugdr::control::make_connect_qp_request(
                                             first.qp_identity, second.qp_num, retry,
                                             ugdr::control::kQpConnectMask)))
                   .response.status == 0 &&
           service.handle(second.session, decoded(ugdr::control::make_connect_qp_request(
                                              second.qp_identity, first.qp_num, retry,
                                              ugdr::control::kQpConnectMask)))
                   .response.status == 0;
}

bool post_send(ugdr::control::QpService &service, const Endpoint &source, const Endpoint &target,
               std::uint64_t wr_id, ugdr_wr_opcode opcode, unsigned int flags,
               std::uint32_t immediate = 0, std::uint32_t first_length = 32,
               std::uint32_t second_length = 16) {
    ugdr::control::WorkerQpView view;
    if (service.worker_qp_view(source.qp_num, &view) != 0) {
        return false;
    }
    std::array<ugdr_sge, 2> sges{{
        {source.memory.client_address, first_length, source.registration.lkey},
        {source.memory.client_address + 128, second_length, source.registration.lkey},
    }};
    ugdr_send_wr wr{};
    wr.wr_id = wr_id;
    wr.sg_list = sges.data();
    wr.num_sge = static_cast<int>(sges.size());
    wr.opcode = opcode;
    wr.send_flags = flags;
    wr.imm_data = immediate;
    wr.wr.rdma.remote_addr = target.memory.client_address + 512;
    wr.wr.rdma.rkey = target.registration.rkey;
    ugdr_send_wr *bad_wr = nullptr;
    return ugdr::api::post_send_chain(*view.send_queue, view.max_send_sge, &wr, &bad_wr) == 0;
}

bool post_receive(ugdr::control::QpService &service, const Endpoint &endpoint,
                  std::uint64_t wr_id) {
    ugdr::control::WorkerQpView view;
    if (service.worker_qp_view(endpoint.qp_num, &view) != 0) {
        return false;
    }
    ugdr_recv_wr wr{};
    wr.wr_id = wr_id;
    ugdr_recv_wr *bad_wr = nullptr;
    return ugdr::api::post_receive_chain(*view.receive_queue, view.max_recv_sge, &wr, &bad_wr) == 0;
}

std::vector<ugdr::queue::CompletionEntry> drain(ugdr::control::QpService &service,
                                                const Endpoint &endpoint) {
    ugdr::control::WorkerQpView view;
    if (service.worker_qp_view(endpoint.qp_num, &view) != 0) {
        return {};
    }
    return ugdr::test::drain_cq(*view.send_cq);
}

bool drive(ugdr::worker::LoopWorker &requester, ugdr::worker::LoopWorker &responder,
           ugdr::test::ScriptedCopyBackend &backend, ugdr::worker::DatagramResult result) {
    bool made_progress = false;
    for (int iteration = 0; iteration < 64; ++iteration) {
        bool progressed = requester.progress_once();
        progressed = responder.progress_once() || progressed;
        if (backend.accepted_count() != 0) {
            progressed = backend.progress_once(result) || progressed;
        }
        progressed = responder.progress_once() || progressed;
        progressed = requester.progress_once() || progressed;
        made_progress = made_progress || progressed;
        if (!progressed) {
            break;
        }
    }
    return made_progress && backend.accepted_count() == 0;
}

class RecordingObserver final : public ugdr::worker::ParentCompletionObserver {
  public:
    void on_parent_completion(const ugdr::worker::ParentCompletionEvent &event) noexcept override {
        events.push_back(event);
    }

    std::vector<ugdr::worker::ParentCompletionEvent> events;
};

bool payload_split_and_aggregate_test() {
    FakeCudaBackend memory_backend;
    ugdr::control::QpService service(memory_backend);
    Endpoint requester_endpoint;
    Endpoint responder_endpoint;
    if (!make_endpoint(service, 401, UINT64_C(0x50000000), &requester_endpoint) ||
        !make_endpoint(service, 402, UINT64_C(0x60000000), &responder_endpoint) ||
        !connect_endpoints(service, requester_endpoint, responder_endpoint)) {
        return false;
    }

    ugdr::worker::LocalTransport transport(8, 8);
    ugdr::test::ScriptedCopyBackend backend(8);
    RecordingObserver observer;
    ugdr::worker::LoopWorker requester(service, requester_endpoint.qp_num, transport, backend,
                                       ugdr::worker::LoopWorkerRole::requester, 10, &observer);
    ugdr::worker::LoopWorker responder(service, responder_endpoint.qp_num, transport, backend,
                                       ugdr::worker::LoopWorkerRole::responder, 10);
    if (!post_send(service, requester_endpoint, responder_endpoint, 41, UGDR_WR_RDMA_WRITE,
                   UGDR_SEND_SIGNALED)) {
        return false;
    }
    for (int iteration = 0; iteration < 16 && backend.accepted_count() != 6; ++iteration) {
        (void)requester.progress_once();
        (void)responder.progress_once();
    }
    if (backend.accepted_count() != 6) {
        return false;
    }

    constexpr std::array<std::uint32_t, 6> expected_lengths{10, 10, 10, 2, 10, 6};
    constexpr std::array<std::uint64_t, 6> expected_offsets{0, 10, 20, 30, 32, 42};
    constexpr std::array<std::size_t, 6> completion_order{5, 0, 3, 1, 1, 0};
    std::array<bool, 6> seen{};
    for (const std::size_t position : completion_order) {
        ugdr::worker::BackendRequest completed;
        if (!backend.progress_at(position, ugdr::worker::DatagramResult::success, &completed) ||
            completed.payload_index >= seen.size() || seen[completed.payload_index] ||
            completed.payload_count != expected_lengths.size() ||
            completed.parent_total_length != 48 ||
            completed.payload_length != expected_lengths[completed.payload_index] ||
            completed.payload_offset != expected_offsets[completed.payload_index]) {
            return false;
        }
        seen[completed.payload_index] = true;
    }
    for (int iteration = 0; iteration < 5; ++iteration) {
        if (!responder.progress_once() || requester.progress_once()) {
            return false;
        }
    }
    if (!responder.progress_once() || !requester.progress_once()) {
        return false;
    }
    auto completions = drain(service, requester_endpoint);
    if (completions.size() != 1 || completions[0].wr_id != 41 ||
        completions[0].status != UGDR_WC_SUCCESS || observer.events.size() != 1 ||
        observer.events[0].wr_id != 41 || observer.events[0].logical_bytes != 48 ||
        observer.events[0].payload_count != 6 ||
        observer.events[0].result != ugdr::worker::DatagramResult::success) {
        return false;
    }

    if (!post_send(service, requester_endpoint, responder_endpoint, 42, UGDR_WR_RDMA_WRITE,
                   UGDR_SEND_SIGNALED, 0, 5, 5) ||
        !drive(requester, responder, backend, ugdr::worker::DatagramResult::success)) {
        return false;
    }
    completions = drain(service, requester_endpoint);
    return completions.size() == 1 && completions[0].wr_id == 42 && observer.events.size() == 2 &&
           observer.events[1].wr_id == 42 && observer.events[1].logical_bytes == 10 &&
           observer.events[1].payload_count == 2;
}

bool deterministic_error_test() {
    FakeCudaBackend memory_backend;
    ugdr::control::QpService service(memory_backend);
    Endpoint requester_endpoint;
    Endpoint responder_endpoint;
    if (!make_endpoint(service, 501, UINT64_C(0x70000000), &requester_endpoint) ||
        !make_endpoint(service, 502, UINT64_C(0x71000000), &responder_endpoint) ||
        !connect_endpoints(service, requester_endpoint, responder_endpoint)) {
        return false;
    }

    ugdr::worker::LocalTransport transport(8, 8);
    ugdr::test::ScriptedCopyBackend backend(8);
    RecordingObserver observer;
    ugdr::worker::LoopWorker requester(service, requester_endpoint.qp_num, transport, backend,
                                       ugdr::worker::LoopWorkerRole::requester, 16, &observer);
    ugdr::worker::LoopWorker responder(service, responder_endpoint.qp_num, transport, backend,
                                       ugdr::worker::LoopWorkerRole::responder, 16);
    if (!post_send(service, requester_endpoint, responder_endpoint, 51, UGDR_WR_RDMA_WRITE, 0)) {
        return false;
    }
    for (int iteration = 0; iteration < 16 && backend.accepted_count() != 3; ++iteration) {
        (void)requester.progress_once();
        (void)responder.progress_once();
    }
    if (backend.accepted_count() != 3 ||
        !backend.progress_at(2, ugdr::worker::DatagramResult::backend_error) ||
        !backend.progress_at(0, ugdr::worker::DatagramResult::remote_access_error) ||
        !backend.progress_at(0, ugdr::worker::DatagramResult::remote_operation_error)) {
        return false;
    }
    for (int iteration = 0; iteration < 16 && observer.events.empty(); ++iteration) {
        (void)responder.progress_once();
        (void)requester.progress_once();
    }
    const auto completions = drain(service, requester_endpoint);
    return observer.events.size() == 1 &&
           observer.events[0].result == ugdr::worker::DatagramResult::remote_access_error &&
           completions.size() == 1 && completions[0].wr_id == 51 &&
           completions[0].status == UGDR_WC_REM_ACCESS_ERR;
}

bool sq_sig_all_test() {
    FakeCudaBackend memory_backend;
    ugdr::control::QpService service(memory_backend);
    Endpoint requester_endpoint;
    Endpoint responder_endpoint;
    if (!make_endpoint(service, 301, UINT64_C(0x30000000), &requester_endpoint, true) ||
        !make_endpoint(service, 302, UINT64_C(0x40000000), &responder_endpoint) ||
        !connect_endpoints(service, requester_endpoint, responder_endpoint)) {
        return false;
    }
    ugdr::worker::LocalTransport transport(2, 2);
    ugdr::test::ScriptedCopyBackend backend(2);
    ugdr::worker::LoopWorker requester(service, requester_endpoint.qp_num, transport, backend,
                                       ugdr::worker::LoopWorkerRole::requester);
    ugdr::worker::LoopWorker responder(service, responder_endpoint.qp_num, transport, backend,
                                       ugdr::worker::LoopWorkerRole::responder);
    if (!post_send(service, requester_endpoint, responder_endpoint, 31, UGDR_WR_RDMA_WRITE, 0) ||
        !drive(requester, responder, backend, ugdr::worker::DatagramResult::success)) {
        return false;
    }
    const auto completions = drain(service, requester_endpoint);
    return completions.size() == 1 && completions[0].wr_id == 31 &&
           completions[0].status == UGDR_WC_SUCCESS;
}

}  // namespace

int main() {
    FakeCudaBackend memory_backend;
    ugdr::control::QpService service(memory_backend);
    Endpoint requester_endpoint;
    Endpoint responder_endpoint;
    if (!make_endpoint(service, 101, UINT64_C(0x10000000), &requester_endpoint) ||
        !make_endpoint(service, 202, UINT64_C(0x20000000), &responder_endpoint) ||
        !connect_endpoints(service, requester_endpoint, responder_endpoint)) {
        return 1;
    }

    ugdr::worker::LocalTransport transport(2, 2);
    ugdr::test::ScriptedCopyBackend backend(2);
    ugdr::worker::LoopWorker requester(service, requester_endpoint.qp_num, transport, backend,
                                       ugdr::worker::LoopWorkerRole::requester);
    ugdr::worker::LoopWorker responder(service, responder_endpoint.qp_num, transport, backend,
                                       ugdr::worker::LoopWorkerRole::responder);

    if (!post_send(service, requester_endpoint, responder_endpoint, 11, UGDR_WR_RDMA_WRITE,
                   UGDR_SEND_SIGNALED) ||
        !requester.progress_once() || !drain(service, requester_endpoint).empty() ||
        !responder.progress_once() || backend.accepted_count() != 1) {
        return 2;
    }
    const auto *submitted = backend.front_request();
    std::uint64_t expected_source = 0;
    std::uint64_t expected_target = 0;
    if (submitted == nullptr || submitted->parent_total_length != 48 ||
        submitted->payload_count != 2 || submitted->payload_index != 0 ||
        submitted->payload_offset != 0 || submitted->payload_length != 32 ||
        service.resolve_lkey(requester_endpoint.session, requester_endpoint.pd_identity,
                             requester_endpoint.registration.lkey,
                             requester_endpoint.memory.client_address, 32, &expected_source) != 0 ||
        service.resolve_rkey(responder_endpoint.session, responder_endpoint.pd_identity,
                             responder_endpoint.registration.rkey,
                             responder_endpoint.memory.client_address + 512, 48,
                             &expected_target) != 0 ||
        submitted->source_daemon_address != expected_source ||
        submitted->target_daemon_address != expected_target || !backend.progress_once() ||
        !drive(requester, responder, backend, ugdr::worker::DatagramResult::success)) {
        return 3;
    }
    auto completions = drain(service, requester_endpoint);
    if (completions.size() != 1 || completions[0].wr_id != 11 ||
        completions[0].status != UGDR_WC_SUCCESS || completions[0].opcode != UGDR_WC_RDMA_WRITE ||
        completions[0].qp_num != requester_endpoint.qp_num) {
        return 4;
    }

    if (!post_send(service, requester_endpoint, responder_endpoint, 12, UGDR_WR_RDMA_WRITE, 0) ||
        !drive(requester, responder, backend, ugdr::worker::DatagramResult::success) ||
        !drain(service, requester_endpoint).empty()) {
        return 5;
    }

    if (!post_receive(service, responder_endpoint, 21) ||
        !post_send(service, requester_endpoint, responder_endpoint, 13, UGDR_WR_RDMA_WRITE_WITH_IMM,
                   UGDR_SEND_SIGNALED, 0x12345678) ||
        !drive(requester, responder, backend, ugdr::worker::DatagramResult::success)) {
        return 6;
    }
    completions = drain(service, requester_endpoint);
    if (completions.size() != 1 || completions[0].wr_id != 13 ||
        completions[0].status != UGDR_WC_SUCCESS) {
        return 7;
    }
    auto receive_completions = drain(service, responder_endpoint);
    if (receive_completions.size() != 1 || receive_completions[0].wr_id != 21 ||
        receive_completions[0].opcode != UGDR_WC_RECV_RDMA_WITH_IMM ||
        receive_completions[0].byte_length != 48 ||
        receive_completions[0].immediate_data != 0x12345678 ||
        receive_completions[0].flags != UGDR_WC_WITH_IMM) {
        return 8;
    }

    if (!post_send(service, requester_endpoint, responder_endpoint, 14, UGDR_WR_RDMA_WRITE_WITH_IMM,
                   UGDR_SEND_SIGNALED, 9) ||
        !requester.progress_once() || !responder.progress_once() || backend.accepted_count() != 0 ||
        responder.progress_once() || !post_receive(service, responder_endpoint, 22) ||
        !drive(requester, responder, backend, ugdr::worker::DatagramResult::success)) {
        return 9;
    }
    completions = drain(service, requester_endpoint);
    receive_completions = drain(service, responder_endpoint);
    if (completions.size() != 1 || completions[0].wr_id != 14 || receive_completions.size() != 1 ||
        receive_completions[0].wr_id != 22) {
        return 10;
    }

    if (!post_send(service, requester_endpoint, responder_endpoint, 15, UGDR_WR_RDMA_WRITE, 0) ||
        !drive(requester, responder, backend, ugdr::worker::DatagramResult::backend_error)) {
        return 11;
    }
    completions = drain(service, requester_endpoint);
    if (completions.size() != 1 || completions[0].wr_id != 15 ||
        completions[0].status != UGDR_WC_GENERAL_ERR) {
        return 12;
    }

    if (!post_send(service, requester_endpoint, responder_endpoint, 17, UGDR_WR_RDMA_WRITE,
                   UGDR_SEND_SIGNALED) ||
        !requester.progress_once() || !responder.progress_once() || !backend.progress_once() ||
        !responder.progress_once() || !requester.progress_once() || !responder.progress_once() ||
        !backend.progress_once() || !responder.progress_once()) {
        return 13;
    }
    ugdr::control::WorkerQpView requester_view;
    if (service.worker_qp_view(requester_endpoint.qp_num, &requester_view) != 0) {
        return 14;
    }
    std::array<ugdr::queue::CompletionEntry, 8> occupying_completions{};
    for (std::size_t index = 0; index < occupying_completions.size(); ++index) {
        occupying_completions[index].wr_id = 1000 + index;
    }
    if (ugdr::queue::produce_completions(*requester_view.send_cq, occupying_completions.data(),
                                         static_cast<int>(occupying_completions.size())) !=
            static_cast<int>(occupying_completions.size()) ||
        !requester.progress_once() ||
        drain(service, requester_endpoint).size() != occupying_completions.size() ||
        !requester.progress_once()) {
        return 15;
    }
    completions = drain(service, requester_endpoint);
    if (completions.size() != 1 || completions[0].wr_id != 17 || requester.progress_once()) {
        return 16;
    }

    backend.set_capacity(0);
    if (!post_send(service, requester_endpoint, responder_endpoint, 18, UGDR_WR_RDMA_WRITE,
                   UGDR_SEND_SIGNALED) ||
        !requester.progress_once() || !responder.progress_once() || backend.accepted_count() != 0) {
        return 17;
    }
    backend.set_capacity(2);
    if (!drive(requester, responder, backend, ugdr::worker::DatagramResult::success)) {
        return 18;
    }
    completions = drain(service, requester_endpoint);
    if (completions.size() != 1 || completions[0].wr_id != 18) {
        return 19;
    }

    if (!post_send(service, requester_endpoint, responder_endpoint, 23, UGDR_WR_RDMA_WRITE,
                   UGDR_SEND_SIGNALED) ||
        !requester.progress_once() || !responder.progress_once() || !backend.progress_once() ||
        !responder.progress_once() || !requester.progress_once() || !responder.progress_once() ||
        !backend.progress_once()) {
        return 20;
    }
    const ugdr::worker::ResponseDatagram blocking_response{
        UINT64_C(0xffffffffffffffff), ugdr::worker::DatagramResult::success, 0};
    if (!transport.try_push_response(blocking_response) ||
        !transport.try_push_response(blocking_response) || !responder.progress_once()) {
        return 21;
    }
    ugdr::worker::ResponseDatagram removed_response;
    if (!transport.try_pop_response(removed_response) ||
        !transport.try_pop_response(removed_response) || !responder.progress_once() ||
        !requester.progress_once()) {
        return 22;
    }
    completions = drain(service, requester_endpoint);
    if (completions.size() != 1 || completions[0].wr_id != 23) {
        return 23;
    }

    if (!post_send(service, requester_endpoint, responder_endpoint, 19, UGDR_WR_RDMA_WRITE,
                   UGDR_SEND_SIGNALED) ||
        !post_send(service, requester_endpoint, responder_endpoint, 20, UGDR_WR_RDMA_WRITE,
                   UGDR_SEND_SIGNALED) ||
        !drive(requester, responder, backend, ugdr::worker::DatagramResult::success)) {
        return 24;
    }
    completions = drain(service, requester_endpoint);
    if (completions.size() != 2 || completions[0].wr_id != 19 || completions[1].wr_id != 20) {
        return 25;
    }

    ugdr::worker::RequestDatagram occupying;
    occupying.parent_request_id = 99;
    ugdr::worker::RequestDatagram occupying_second;
    occupying_second.parent_request_id = 100;
    if (!transport.try_push_request(occupying) || !transport.try_push_request(occupying_second) ||
        !post_send(service, requester_endpoint, responder_endpoint, 16, UGDR_WR_RDMA_WRITE,
                   UGDR_SEND_SIGNALED) ||
        requester.progress_once()) {
        return 27;
    }
    ugdr::worker::RequestDatagram removed;
    if (service.worker_qp_view(requester_endpoint.qp_num, &requester_view) != 0) {
        return 28;
    }
    if (!transport.try_pop_request(removed) || !transport.try_pop_request(removed)) {
        return 31;
    }
    if (!drive(requester, responder, backend, ugdr::worker::DatagramResult::success)) {
        return 32;
    }
    completions = drain(service, requester_endpoint);
    return completions.size() == 1 && completions[0].wr_id == 16 && sq_sig_all_test() &&
                   payload_split_and_aggregate_test() && deterministic_error_test()
               ? 0
               : 29;
}

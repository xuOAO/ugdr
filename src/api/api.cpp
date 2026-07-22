#include "ugdr/api.hpp"

#include "api/wr_posting.hpp"
#include "control/device_context.hpp"
#include "control/pd_mr_cq.hpp"
#include "control/qp.hpp"
#include "gpu/cuda_ipc_memory.hpp"
#include "queue/descriptors.hpp"
#include "queue/shared_ring.hpp"

#include <cerrno>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct ugdr_device {
    std::uint64_t catalog_identity = 0;
    std::uint64_t connection_epoch = 0;
    bool live = false;
};

struct ugdr_context {
    std::uint64_t daemon_identity = 0;
    std::uint64_t connection_epoch = 0;
    bool live = false;
};

struct ugdr_pd {
    ugdr_context *context = nullptr;
    std::uint64_t daemon_identity = 0;
    std::uint64_t connection_epoch = 0;
    bool live = false;
};

struct ugdr_cq {
    ugdr_context *context = nullptr;
    void *cq_context = nullptr;
    std::uint64_t daemon_identity = 0;
    std::uint64_t connection_epoch = 0;
    int cqe = 0;
    bool live = false;
    std::mutex polling_mutex;
    ugdr::queue::SharedRing completions;
};

struct ugdr_qp {
    ugdr_pd *pd = nullptr;
    ugdr_qp_init_attr init_attr{};
    std::uint64_t daemon_identity = 0;
    std::uint64_t connection_epoch = 0;
    ugdr_qp_state cached_state = UGDR_QPS_RESET;
    bool live = false;
    std::mutex posting_mutex;
    ugdr::queue::SharedRing send_queue;
    ugdr::queue::SharedRing receive_queue;
};

namespace {

struct DeviceListRecord {
    std::unique_ptr<ugdr_device *[]> pointers;
    std::vector<ugdr_device *> devices;
    bool live = true;
};

struct MrProxyRecord {
    ugdr_mr value{};
    std::uint64_t daemon_identity = 0;
    std::uint64_t connection_epoch = 0;
    bool live = false;
};

class ClientRuntime {
  public:
    ugdr_device **get_device_list(int *num_devices) {
        std::lock_guard lock(mutex_);
        const int connect_status = ensure_connected();
        if (connect_status != 0) {
            errno = connect_status;
            return nullptr;
        }

        std::vector<ugdr::control::DeviceDescriptor> descriptors;
        const int list_status = client_.list_devices(&descriptors);
        if (list_status != 0) {
            errno = list_status;
            return nullptr;
        }

        auto list = std::make_unique<DeviceListRecord>();
        list->pointers = std::make_unique<ugdr_device *[]>(descriptors.size() + 1);
        list->devices.reserve(descriptors.size());
        for (std::size_t index = 0; index < descriptors.size(); ++index) {
            auto device = std::make_unique<ugdr_device>();
            device->catalog_identity = descriptors[index].identity;
            device->connection_epoch = client_.connection_epoch();
            device->live = true;
            ugdr_device *const pointer = device.get();
            devices_.insert(pointer);
            device_storage_.push_back(std::move(device));
            list->devices.push_back(pointer);
            list->pointers[index] = pointer;
        }
        list->pointers[descriptors.size()] = nullptr;
        ugdr_device **const result = list->pointers.get();
        lists_.emplace(result, list.get());
        list_storage_.push_back(std::move(list));
        if (num_devices != nullptr) {
            *num_devices = static_cast<int>(descriptors.size());
        }
        return result;
    }

    void free_device_list(ugdr_device **list) {
        std::lock_guard lock(mutex_);
        const auto found = lists_.find(list);
        if (found == lists_.end() || !found->second->live) {
            errno = EINVAL;
            return;
        }
        found->second->live = false;
        for (ugdr_device *device : found->second->devices) {
            device->live = false;
        }
    }

    ugdr_context *open_device(ugdr_device *device) {
        std::lock_guard lock(mutex_);
        if (devices_.find(device) == devices_.end() || !device->live) {
            errno = EINVAL;
            return nullptr;
        }
        const int connect_status = ensure_connected();
        if (connect_status != 0) {
            errno = connect_status;
            return nullptr;
        }
        if (device->connection_epoch != client_.connection_epoch()) {
            device->live = false;
            errno = EINVAL;
            return nullptr;
        }
        std::uint64_t identity = 0;
        const int create_status = client_.create_context(device->catalog_identity, &identity);
        if (create_status != 0) {
            errno = create_status;
            return nullptr;
        }
        auto context = std::make_unique<ugdr_context>();
        context->daemon_identity = identity;
        context->connection_epoch = client_.connection_epoch();
        context->live = true;
        ugdr_context *const result = context.get();
        contexts_.insert(result);
        context_storage_.push_back(std::move(context));
        return result;
    }

    int close_device(ugdr_context *context) {
        std::lock_guard lock(mutex_);
        if (contexts_.find(context) == contexts_.end() || !context->live) {
            errno = EINVAL;
            return -1;
        }
        const int connect_status = ensure_connected();
        if (connect_status != 0) {
            errno = connect_status;
            return -1;
        }
        if (context->connection_epoch != client_.connection_epoch()) {
            context->live = false;
            errno = EINVAL;
            return -1;
        }
        const int destroy_status = client_.destroy_context(context->daemon_identity);
        if (destroy_status != 0) {
            errno = destroy_status;
            return -1;
        }
        context->live = false;
        return 0;
    }

    ugdr_pd *alloc_pd(ugdr_context *context) {
        std::lock_guard lock(mutex_);
        if (contexts_.find(context) == contexts_.end() || !context->live) {
            errno = EINVAL;
            return nullptr;
        }
        const int connect_status = ensure_connected();
        if (connect_status != 0) {
            errno = connect_status;
            return nullptr;
        }
        if (context->connection_epoch != client_.connection_epoch()) {
            context->live = false;
            errno = EINVAL;
            return nullptr;
        }
        auto pd = std::make_unique<ugdr_pd>();
        std::uint64_t identity = 0;
        const int create_status =
            ugdr::control::client_create_pd(client_, context->daemon_identity, &identity);
        if (create_status != 0) {
            errno = create_status;
            return nullptr;
        }
        pd->context = context;
        pd->daemon_identity = identity;
        pd->connection_epoch = client_.connection_epoch();
        pd->live = true;
        ugdr_pd *const result = pd.get();
        try {
            pd_storage_.push_back(std::move(pd));
            pds_.insert(result);
        } catch (...) {
            result->live = false;
            (void)ugdr::control::client_destroy_pd(client_, identity);
            throw;
        }
        return result;
    }

    int dealloc_pd(ugdr_pd *pd) {
        std::lock_guard lock(mutex_);
        if (pds_.find(pd) == pds_.end() || !pd->live) {
            return EINVAL;
        }
        const int connect_status = ensure_connected();
        if (connect_status != 0) {
            return connect_status;
        }
        if (pd->connection_epoch != client_.connection_epoch()) {
            pd->live = false;
            return EINVAL;
        }
        const int destroy_status = ugdr::control::client_destroy_pd(client_, pd->daemon_identity);
        if (destroy_status == 0) {
            pd->live = false;
        }
        return destroy_status;
    }

    ugdr_mr *reg_mr(ugdr_pd *pd, void *address, std::size_t length, int access) {
        std::lock_guard lock(mutex_);
        if (pds_.find(pd) == pds_.end() || !pd->live || address == nullptr || length == 0 ||
            access < 0 ||
            (static_cast<std::uint32_t>(access) &
             ~(ugdr::control::kAccessLocalWrite | ugdr::control::kAccessRemoteWrite)) != 0 ||
            ((static_cast<std::uint32_t>(access) & ugdr::control::kAccessRemoteWrite) != 0 &&
             (static_cast<std::uint32_t>(access) & ugdr::control::kAccessLocalWrite) == 0)) {
            errno = EINVAL;
            return nullptr;
        }
        const int connect_status = ensure_connected();
        if (connect_status != 0) {
            errno = connect_status;
            return nullptr;
        }
        if (pd->connection_epoch != client_.connection_epoch()) {
            pd->live = false;
            errno = EINVAL;
            return nullptr;
        }
        if (next_mr_handle_ > std::numeric_limits<std::uint32_t>::max()) {
            errno = ENOSPC;
            return nullptr;
        }

        ugdr::gpu::ExportedCudaMemory memory;
        const int export_status = ugdr::gpu::export_cuda_memory(address, length, &memory);
        if (export_status != 0) {
            errno = export_status;
            return nullptr;
        }
        auto record = std::make_unique<MrProxyRecord>();
        std::uint64_t identity = 0;
        ugdr::control::MrRegistrationResult accepted;
        const int register_status = ugdr::control::client_register_mr(
            client_, pd->daemon_identity, memory, static_cast<std::uint32_t>(access), &identity,
            &accepted);
        if (register_status != 0) {
            errno = register_status;
            return nullptr;
        }

        record->value.context = pd->context;
        record->value.pd = pd;
        record->value.addr = address;
        record->value.length = length;
        record->value.handle = static_cast<std::uint32_t>(next_mr_handle_++);
        record->value.lkey = accepted.lkey;
        record->value.rkey = accepted.rkey;
        record->daemon_identity = identity;
        record->connection_epoch = client_.connection_epoch();
        record->live = true;
        MrProxyRecord *const record_pointer = record.get();
        ugdr_mr *const result = &record->value;
        try {
            mr_storage_.push_back(std::move(record));
            mrs_.emplace(result, mr_storage_.back().get());
        } catch (...) {
            record_pointer->live = false;
            (void)ugdr::control::client_deregister_mr(client_, identity);
            throw;
        }
        return result;
    }

    int dereg_mr(ugdr_mr *mr) {
        std::lock_guard lock(mutex_);
        const auto found = mrs_.find(mr);
        if (found == mrs_.end() || !found->second->live) {
            return EINVAL;
        }
        const int connect_status = ensure_connected();
        if (connect_status != 0) {
            return connect_status;
        }
        MrProxyRecord *const record = found->second;
        if (record->connection_epoch != client_.connection_epoch()) {
            record->live = false;
            return EINVAL;
        }
        const int deregister_status =
            ugdr::control::client_deregister_mr(client_, record->daemon_identity);
        if (deregister_status == 0) {
            record->live = false;
        }
        return deregister_status;
    }

    ugdr_cq *create_cq(ugdr_context *context, int cqe, void *cq_context, ugdr_comp_channel *channel,
                       int comp_vector) {
        std::lock_guard lock(mutex_);
        if (contexts_.find(context) == contexts_.end() || !context->live || cqe <= 0 ||
            channel != nullptr || comp_vector != 0) {
            errno = EINVAL;
            return nullptr;
        }
        const int connect_status = ensure_connected();
        if (connect_status != 0) {
            errno = connect_status;
            return nullptr;
        }
        if (context->connection_epoch != client_.connection_epoch()) {
            context->live = false;
            errno = EINVAL;
            return nullptr;
        }
        auto cq = std::make_unique<ugdr_cq>();
        std::uint64_t identity = 0;
        const int create_status = ugdr::control::client_create_cq(client_, context->daemon_identity,
                                                                  static_cast<std::uint32_t>(cqe),
                                                                  &identity, &cq->completions);
        if (create_status != 0) {
            errno = create_status;
            return nullptr;
        }
        cq->context = context;
        cq->cq_context = cq_context;
        cq->daemon_identity = identity;
        cq->connection_epoch = client_.connection_epoch();
        cq->cqe = cqe;
        cq->live = true;
        ugdr_cq *const result = cq.get();
        try {
            cq_storage_.push_back(std::move(cq));
            std::unique_lock registry_lock(cq_registry_mutex_);
            cqs_.insert(result);
        } catch (...) {
            result->live = false;
            (void)ugdr::control::client_destroy_cq(client_, identity);
            throw;
        }
        return result;
    }

    int destroy_cq(ugdr_cq *cq) {
        std::lock_guard lock(mutex_);
        if (cqs_.find(cq) == cqs_.end()) {
            return EINVAL;
        }
        std::lock_guard polling_lock(cq->polling_mutex);
        if (!cq->live) {
            return EINVAL;
        }
        const int connect_status = ensure_connected();
        if (connect_status != 0) {
            return connect_status;
        }
        if (cq->connection_epoch != client_.connection_epoch()) {
            cq->live = false;
            cq->completions.reset();
            return EINVAL;
        }
        const int destroy_status = ugdr::control::client_destroy_cq(client_, cq->daemon_identity);
        if (destroy_status == 0) {
            cq->live = false;
            cq->completions.reset();
        }
        return destroy_status;
    }

    int poll_cq(ugdr_cq *cq, int num_entries, ugdr_wc *wc) noexcept {
        if (cq == nullptr || num_entries < 0 || (num_entries > 0 && wc == nullptr)) {
            return -EINVAL;
        }
        {
            std::shared_lock registry_lock(cq_registry_mutex_);
            if (cqs_.find(cq) == cqs_.end()) {
                return -EINVAL;
            }
        }
        std::lock_guard polling_lock(cq->polling_mutex);
        if (!cq->live) {
            return -EINVAL;
        }
        if (num_entries == 0) {
            return 0;
        }

        ugdr::queue::ConstSlotBatch batch;
        const int peek_status =
            cq->completions.consumer_peek(static_cast<std::uint32_t>(num_entries), &batch);
        if (peek_status == EAGAIN) {
            return 0;
        }
        if (peek_status != 0) {
            return -peek_status;
        }

        std::size_t output_index = 0;
        const auto copy_span = [&](ugdr::queue::ConstSlotSpan span) {
            const auto *slots = static_cast<const std::byte *>(span.data);
            for (std::uint32_t index = 0; index < span.count; ++index) {
                ugdr::queue::CompletionEntry entry;
                std::memcpy(&entry,
                            slots + static_cast<std::size_t>(index) *
                                        cq->completions.descriptor().slot_stride,
                            sizeof(entry));
                ugdr_wc completion{};
                completion.wr_id = entry.wr_id;
                completion.status = static_cast<ugdr_wc_status>(entry.status);
                completion.opcode = static_cast<ugdr_wc_opcode>(entry.opcode);
                completion.byte_len = entry.byte_length;
                completion.imm_data = entry.immediate_data;
                completion.qp_num = entry.qp_num;
                completion.wc_flags = entry.flags;
                wc[output_index++] = completion;
            }
        };
        copy_span(batch.first);
        copy_span(batch.second);
        const int release_status = cq->completions.consumer_release(batch.count);
        return release_status == 0 ? static_cast<int>(batch.count) : -release_status;
    }

    ugdr_qp *create_qp(ugdr_pd *pd, ugdr_qp_init_attr *init_attr) {
        std::lock_guard lock(mutex_);
        if (pds_.find(pd) == pds_.end() || !pd->live || init_attr == nullptr ||
            cqs_.find(init_attr->send_cq) == cqs_.end() ||
            cqs_.find(init_attr->recv_cq) == cqs_.end() || !init_attr->send_cq->live ||
            !init_attr->recv_cq->live || pd->context != init_attr->send_cq->context ||
            pd->context != init_attr->recv_cq->context || init_attr->max_send_wr == 0 ||
            init_attr->max_recv_wr == 0 || init_attr->max_send_sge == 0 ||
            init_attr->max_recv_sge == 0 || init_attr->qp_type != UGDR_QPT_RC ||
            (init_attr->sq_sig_all != 0 && init_attr->sq_sig_all != 1)) {
            errno = EINVAL;
            return nullptr;
        }
        const int connect_status = ensure_connected();
        if (connect_status != 0) {
            errno = connect_status;
            return nullptr;
        }
        const std::uint64_t epoch = client_.connection_epoch();
        if (pd->connection_epoch != epoch || init_attr->send_cq->connection_epoch != epoch ||
            init_attr->recv_cq->connection_epoch != epoch) {
            pd->live = false;
            if (init_attr->send_cq == init_attr->recv_cq) {
                std::lock_guard polling_lock(init_attr->send_cq->polling_mutex);
                init_attr->send_cq->live = false;
            } else {
                std::scoped_lock polling_locks(init_attr->send_cq->polling_mutex,
                                               init_attr->recv_cq->polling_mutex);
                init_attr->send_cq->live = false;
                init_attr->recv_cq->live = false;
            }
            errno = EINVAL;
            return nullptr;
        }

        ugdr::control::QpCreateAttributes attributes;
        attributes.send_cq_identity = init_attr->send_cq->daemon_identity;
        attributes.recv_cq_identity = init_attr->recv_cq->daemon_identity;
        attributes.max_send_wr = init_attr->max_send_wr;
        attributes.max_recv_wr = init_attr->max_recv_wr;
        attributes.max_send_sge = init_attr->max_send_sge;
        attributes.max_recv_sge = init_attr->max_recv_sge;
        attributes.qp_type = static_cast<std::uint32_t>(init_attr->qp_type);
        attributes.sq_sig_all = static_cast<std::uint32_t>(init_attr->sq_sig_all);

        auto qp = std::make_unique<ugdr_qp>();
        std::uint64_t identity = 0;
        const int create_status =
            ugdr::control::client_create_qp(client_, pd->daemon_identity, attributes, &identity,
                                            &qp->send_queue, &qp->receive_queue);
        if (create_status != 0) {
            errno = create_status;
            return nullptr;
        }
        qp->pd = pd;
        qp->init_attr = *init_attr;
        qp->daemon_identity = identity;
        qp->connection_epoch = epoch;
        qp->cached_state = UGDR_QPS_RESET;
        qp->live = true;
        ugdr_qp *const result = qp.get();
        try {
            qp_storage_.push_back(std::move(qp));
            std::unique_lock registry_lock(qp_registry_mutex_);
            qps_.insert(result);
        } catch (...) {
            result->live = false;
            (void)ugdr::control::client_destroy_qp(client_, identity);
            throw;
        }
        return result;
    }

    int destroy_qp(ugdr_qp *qp) {
        std::lock_guard lock(mutex_);
        if (qps_.find(qp) == qps_.end()) {
            return EINVAL;
        }
        std::lock_guard posting_lock(qp->posting_mutex);
        if (!qp->live) {
            return EINVAL;
        }
        const int connect_status = ensure_connected();
        if (connect_status != 0) {
            return connect_status;
        }
        if (qp->connection_epoch != client_.connection_epoch()) {
            qp->live = false;
            qp->send_queue.reset();
            qp->receive_queue.reset();
            return EINVAL;
        }
        const int destroy_status = ugdr::control::client_destroy_qp(client_, qp->daemon_identity);
        if (destroy_status == 0) {
            qp->live = false;
            qp->send_queue.reset();
            qp->receive_queue.reset();
        }
        return destroy_status;
    }

    int modify_qp(ugdr_qp *qp, const ugdr_qp_attr *attr, int attr_mask) {
        std::lock_guard lock(mutex_);
        if (qps_.find(qp) == qps_.end() || attr == nullptr || attr_mask < 0) {
            return EINVAL;
        }
        std::lock_guard posting_lock(qp->posting_mutex);
        if (!qp->live) {
            return EINVAL;
        }
        const int connect_status = ensure_connected();
        if (connect_status != 0) {
            return connect_status;
        }
        if (qp->connection_epoch != client_.connection_epoch()) {
            qp->live = false;
            qp->send_queue.reset();
            qp->receive_queue.reset();
            return EINVAL;
        }
        ugdr::control::QpAttributes attributes;
        attributes.state = static_cast<std::uint32_t>(attr->qp_state);
        attributes.current_state = static_cast<std::uint32_t>(attr->cur_qp_state);
        attributes.access_flags = static_cast<std::uint32_t>(attr->qp_access_flags);
        attributes.timeout = attr->timeout;
        attributes.retry_count = attr->retry_cnt;
        attributes.rnr_retry = attr->rnr_retry;
        attributes.min_rnr_timer = attr->min_rnr_timer;
        const int status = ugdr::control::client_modify_qp(client_, qp->daemon_identity, attributes,
                                                           static_cast<std::uint32_t>(attr_mask));
        if (status == 0 &&
            (static_cast<std::uint32_t>(attr_mask) & ugdr::control::kQpMaskState) != 0) {
            qp->cached_state = attr->qp_state;
        }
        return status;
    }

    int query_qp(ugdr_qp *qp, ugdr_qp_attr *attr, int attr_mask, ugdr_qp_init_attr *init_attr) {
        std::lock_guard lock(mutex_);
        if (qps_.find(qp) == qps_.end() || attr == nullptr || init_attr == nullptr ||
            attr_mask < 0) {
            return EINVAL;
        }
        std::lock_guard posting_lock(qp->posting_mutex);
        if (!qp->live) {
            return EINVAL;
        }
        const int connect_status = ensure_connected();
        if (connect_status != 0) {
            return connect_status;
        }
        if (qp->connection_epoch != client_.connection_epoch()) {
            qp->live = false;
            qp->send_queue.reset();
            qp->receive_queue.reset();
            return EINVAL;
        }
        ugdr::control::QpSnapshot snapshot;
        const int status = ugdr::control::client_query_qp(
            client_, qp->daemon_identity, static_cast<std::uint32_t>(attr_mask), &snapshot);
        if (status != 0) {
            return status;
        }
        ugdr_qp_attr result = *attr;
        const auto mask = static_cast<std::uint32_t>(attr_mask);
        if ((mask & ugdr::control::kQpMaskState) != 0) {
            result.qp_state = static_cast<ugdr_qp_state>(snapshot.attributes.state);
            qp->cached_state = result.qp_state;
        }
        if ((mask & ugdr::control::kQpMaskCurrentState) != 0) {
            result.cur_qp_state = static_cast<ugdr_qp_state>(snapshot.attributes.current_state);
        }
        if ((mask & ugdr::control::kQpMaskAccess) != 0) {
            result.qp_access_flags = static_cast<int>(snapshot.attributes.access_flags);
        }
        if ((mask & ugdr::control::kQpMaskTimeout) != 0) {
            result.timeout = snapshot.attributes.timeout;
        }
        if ((mask & ugdr::control::kQpMaskRetryCount) != 0) {
            result.retry_cnt = snapshot.attributes.retry_count;
        }
        if ((mask & ugdr::control::kQpMaskRnrRetry) != 0) {
            result.rnr_retry = snapshot.attributes.rnr_retry;
        }
        if ((mask & ugdr::control::kQpMaskMinRnrTimer) != 0) {
            result.min_rnr_timer = snapshot.attributes.min_rnr_timer;
        }
        ugdr_qp_init_attr creation = qp->init_attr;
        creation.max_send_wr = snapshot.creation.max_send_wr;
        creation.max_recv_wr = snapshot.creation.max_recv_wr;
        creation.max_send_sge = snapshot.creation.max_send_sge;
        creation.max_recv_sge = snapshot.creation.max_recv_sge;
        creation.qp_type = static_cast<ugdr_qp_type>(snapshot.creation.qp_type);
        creation.sq_sig_all = static_cast<int>(snapshot.creation.sq_sig_all);
        *attr = result;
        *init_attr = creation;
        return 0;
    }

    int query_qp_conn_info(ugdr_qp *qp, ugdr_qp_conn_info *info) {
        std::lock_guard lock(mutex_);
        if (qps_.find(qp) == qps_.end() || info == nullptr) {
            return EINVAL;
        }
        std::lock_guard posting_lock(qp->posting_mutex);
        if (!qp->live) {
            return EINVAL;
        }
        const int connect_status = ensure_connected();
        if (connect_status != 0) {
            return connect_status;
        }
        if (qp->connection_epoch != client_.connection_epoch()) {
            qp->live = false;
            qp->send_queue.reset();
            qp->receive_queue.reset();
            return EINVAL;
        }
        std::uint32_t qp_num = 0;
        const int status =
            ugdr::control::client_query_qp_conn_info(client_, qp->daemon_identity, &qp_num);
        if (status == 0) {
            info->qp_num = qp_num;
        }
        return status;
    }

    int connect_qp(ugdr_qp *qp, const ugdr_qp_conn_info *remote_info, const ugdr_qp_attr *attr,
                   int attr_mask) {
        std::lock_guard lock(mutex_);
        if (qps_.find(qp) == qps_.end() || remote_info == nullptr || attr == nullptr ||
            attr_mask < 0 || remote_info->qp_num == 0) {
            return EINVAL;
        }
        std::lock_guard posting_lock(qp->posting_mutex);
        if (!qp->live) {
            return EINVAL;
        }
        const int connect_status = ensure_connected();
        if (connect_status != 0) {
            return connect_status;
        }
        if (qp->connection_epoch != client_.connection_epoch()) {
            qp->live = false;
            qp->send_queue.reset();
            qp->receive_queue.reset();
            return EINVAL;
        }
        ugdr::control::QpAttributes attributes;
        attributes.timeout = attr->timeout;
        attributes.retry_count = attr->retry_cnt;
        attributes.rnr_retry = attr->rnr_retry;
        attributes.min_rnr_timer = attr->min_rnr_timer;
        const int status =
            ugdr::control::client_connect_qp(client_, qp->daemon_identity, remote_info->qp_num,
                                             attributes, static_cast<std::uint32_t>(attr_mask));
        if (status == 0) {
            qp->cached_state = UGDR_QPS_RTS;
        }
        return status;
    }

    int post_send(ugdr_qp *qp, ugdr_send_wr *wr, ugdr_send_wr **bad_wr) noexcept {
        if (qp == nullptr || wr == nullptr || bad_wr == nullptr) {
            if (wr != nullptr && bad_wr != nullptr) {
                *bad_wr = wr;
            }
            return EINVAL;
        }
        {
            std::shared_lock registry_lock(qp_registry_mutex_);
            if (qps_.find(qp) == qps_.end()) {
                *bad_wr = wr;
                return EINVAL;
            }
        }
        std::lock_guard posting_lock(qp->posting_mutex);
        if (!qp->live || qp->cached_state != UGDR_QPS_RTS) {
            *bad_wr = wr;
            return EINVAL;
        }
        return ugdr::api::post_send_chain(qp->send_queue, qp->init_attr.max_send_sge, wr, bad_wr);
    }

    int post_receive(ugdr_qp *qp, ugdr_recv_wr *wr, ugdr_recv_wr **bad_wr) noexcept {
        if (qp == nullptr || wr == nullptr || bad_wr == nullptr) {
            if (wr != nullptr && bad_wr != nullptr) {
                *bad_wr = wr;
            }
            return EINVAL;
        }
        {
            std::shared_lock registry_lock(qp_registry_mutex_);
            if (qps_.find(qp) == qps_.end()) {
                *bad_wr = wr;
                return EINVAL;
            }
        }
        std::lock_guard posting_lock(qp->posting_mutex);
        if (!qp->live || (qp->cached_state != UGDR_QPS_INIT && qp->cached_state != UGDR_QPS_RTR &&
                          qp->cached_state != UGDR_QPS_RTS)) {
            *bad_wr = wr;
            return EINVAL;
        }
        return ugdr::api::post_receive_chain(qp->receive_queue, qp->init_attr.max_recv_sge, wr,
                                             bad_wr);
    }

  private:
    int ensure_connected() {
        if (client_.connected()) {
            return 0;
        }
        const char *const configured = std::getenv("UGDR_DAEMON_SOCKET");
        const std::string path = configured != nullptr && configured[0] != '\0'
                                     ? configured
                                     : ugdr::control::kDefaultDaemonSocket;
        return client_.connect(path);
    }

    std::mutex mutex_;
    std::shared_mutex cq_registry_mutex_;
    std::shared_mutex qp_registry_mutex_;
    ugdr::control::ControlClient client_;
    std::vector<std::unique_ptr<DeviceListRecord>> list_storage_;
    std::vector<std::unique_ptr<ugdr_device>> device_storage_;
    std::vector<std::unique_ptr<ugdr_context>> context_storage_;
    std::vector<std::unique_ptr<ugdr_pd>> pd_storage_;
    std::vector<std::unique_ptr<MrProxyRecord>> mr_storage_;
    std::vector<std::unique_ptr<ugdr_cq>> cq_storage_;
    std::vector<std::unique_ptr<ugdr_qp>> qp_storage_;
    std::unordered_map<ugdr_device **, DeviceListRecord *> lists_;
    std::unordered_set<ugdr_device *> devices_;
    std::unordered_set<ugdr_context *> contexts_;
    std::unordered_set<ugdr_pd *> pds_;
    std::unordered_map<ugdr_mr *, MrProxyRecord *> mrs_;
    std::unordered_set<ugdr_cq *> cqs_;
    std::unordered_set<ugdr_qp *> qps_;
    std::uint64_t next_mr_handle_ = 1;
};

ClientRuntime &runtime() {
    static ClientRuntime value;
    return value;
}

}  // namespace

extern "C" {

ugdr_device **ugdr_get_device_list(int *num_devices) noexcept {
    try {
        return runtime().get_device_list(num_devices);
    } catch (...) {
        errno = ENOMEM;
        return nullptr;
    }
}

void ugdr_free_device_list(ugdr_device **list) noexcept {
    try {
        runtime().free_device_list(list);
    } catch (...) {
        errno = EINVAL;
    }
}

ugdr_context *ugdr_open_device(ugdr_device *device) noexcept {
    try {
        return runtime().open_device(device);
    } catch (...) {
        errno = ENOMEM;
        return nullptr;
    }
}

int ugdr_close_device(ugdr_context *context) noexcept {
    try {
        return runtime().close_device(context);
    } catch (...) {
        errno = ENOMEM;
        return -1;
    }
}

ugdr_pd *ugdr_alloc_pd(ugdr_context *context) noexcept {
    try {
        return runtime().alloc_pd(context);
    } catch (...) {
        errno = ENOMEM;
        return nullptr;
    }
}

int ugdr_dealloc_pd(ugdr_pd *pd) noexcept {
    try {
        return runtime().dealloc_pd(pd);
    } catch (...) {
        return ENOMEM;
    }
}

ugdr_mr *ugdr_reg_mr(ugdr_pd *pd, void *address, size_t length, int access) noexcept {
    try {
        return runtime().reg_mr(pd, address, length, access);
    } catch (...) {
        errno = ENOMEM;
        return nullptr;
    }
}

int ugdr_dereg_mr(ugdr_mr *mr) noexcept {
    try {
        return runtime().dereg_mr(mr);
    } catch (...) {
        return ENOMEM;
    }
}

ugdr_cq *ugdr_create_cq(ugdr_context *context, int cqe, void *cq_context,
                        ugdr_comp_channel *channel, int comp_vector) noexcept {
    try {
        return runtime().create_cq(context, cqe, cq_context, channel, comp_vector);
    } catch (...) {
        errno = ENOMEM;
        return nullptr;
    }
}

int ugdr_destroy_cq(ugdr_cq *cq) noexcept {
    try {
        return runtime().destroy_cq(cq);
    } catch (...) {
        return ENOMEM;
    }
}

int ugdr_poll_cq(ugdr_cq *cq, int num_entries, ugdr_wc *wc) noexcept {
    if (num_entries < 0 || (num_entries > 0 && wc == nullptr)) {
        return -EINVAL;
    }
    try {
        return runtime().poll_cq(cq, num_entries, wc);
    } catch (...) {
        return -ENOMEM;
    }
}

ugdr_qp *ugdr_create_qp(ugdr_pd *pd, ugdr_qp_init_attr *init_attr) noexcept {
    try {
        return runtime().create_qp(pd, init_attr);
    } catch (...) {
        errno = ENOMEM;
        return nullptr;
    }
}

int ugdr_destroy_qp(ugdr_qp *qp) noexcept {
    try {
        return runtime().destroy_qp(qp);
    } catch (...) {
        return ENOMEM;
    }
}

int ugdr_modify_qp(ugdr_qp *qp, ugdr_qp_attr *attr, int attr_mask) noexcept {
    try {
        return runtime().modify_qp(qp, attr, attr_mask);
    } catch (...) {
        return ENOMEM;
    }
}

int ugdr_query_qp(ugdr_qp *qp, ugdr_qp_attr *attr, int attr_mask,
                  ugdr_qp_init_attr *init_attr) noexcept {
    try {
        return runtime().query_qp(qp, attr, attr_mask, init_attr);
    } catch (...) {
        return ENOMEM;
    }
}

int ugdr_query_qp_conn_info(ugdr_qp *qp, ugdr_qp_conn_info *info) noexcept {
    try {
        return runtime().query_qp_conn_info(qp, info);
    } catch (...) {
        return ENOMEM;
    }
}

int ugdr_connect_qp(ugdr_qp *qp, const ugdr_qp_conn_info *remote_info, const ugdr_qp_attr *attr,
                    int attr_mask) noexcept {
    try {
        return runtime().connect_qp(qp, remote_info, attr, attr_mask);
    } catch (...) {
        return ENOMEM;
    }
}

int ugdr_post_send(ugdr_qp *qp, ugdr_send_wr *wr, ugdr_send_wr **bad_wr) noexcept {
    return runtime().post_send(qp, wr, bad_wr);
}

int ugdr_post_recv(ugdr_qp *qp, ugdr_recv_wr *wr, ugdr_recv_wr **bad_wr) noexcept {
    return runtime().post_receive(qp, wr, bad_wr);
}

}  // extern "C"

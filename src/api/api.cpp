#include "ugdr/api.hpp"

#include "control/device_context.hpp"
#include "control/pd_mr_cq.hpp"
#include "gpu/cuda_ipc_memory.hpp"

#include <cerrno>

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
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
        const int create_status = ugdr::control::client_create_cq(
            client_, context->daemon_identity, static_cast<std::uint32_t>(cqe), &identity);
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
        if (cqs_.find(cq) == cqs_.end() || !cq->live) {
            return EINVAL;
        }
        const int connect_status = ensure_connected();
        if (connect_status != 0) {
            return connect_status;
        }
        if (cq->connection_epoch != client_.connection_epoch()) {
            cq->live = false;
            return EINVAL;
        }
        const int destroy_status = ugdr::control::client_destroy_cq(client_, cq->daemon_identity);
        if (destroy_status == 0) {
            cq->live = false;
        }
        return destroy_status;
    }

    int poll_cq(ugdr_cq *cq) {
        std::lock_guard lock(mutex_);
        if (cqs_.find(cq) == cqs_.end() || !cq->live) {
            return -EINVAL;
        }
        const int connect_status = ensure_connected();
        if (connect_status != 0) {
            return -connect_status;
        }
        if (cq->connection_epoch != client_.connection_epoch()) {
            cq->live = false;
            return -EINVAL;
        }
        return -EOPNOTSUPP;
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
    ugdr::control::ControlClient client_;
    std::vector<std::unique_ptr<DeviceListRecord>> list_storage_;
    std::vector<std::unique_ptr<ugdr_device>> device_storage_;
    std::vector<std::unique_ptr<ugdr_context>> context_storage_;
    std::vector<std::unique_ptr<ugdr_pd>> pd_storage_;
    std::vector<std::unique_ptr<MrProxyRecord>> mr_storage_;
    std::vector<std::unique_ptr<ugdr_cq>> cq_storage_;
    std::unordered_map<ugdr_device **, DeviceListRecord *> lists_;
    std::unordered_set<ugdr_device *> devices_;
    std::unordered_set<ugdr_context *> contexts_;
    std::unordered_set<ugdr_pd *> pds_;
    std::unordered_map<ugdr_mr *, MrProxyRecord *> mrs_;
    std::unordered_set<ugdr_cq *> cqs_;
    std::uint64_t next_mr_handle_ = 1;
};

ClientRuntime &runtime() {
    static ClientRuntime value;
    return value;
}

template <typename T> T *unsupported_pointer() noexcept {
    errno = EOPNOTSUPP;
    return nullptr;
}

constexpr int kUnsupported = EOPNOTSUPP;

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
        return runtime().poll_cq(cq);
    } catch (...) {
        return -ENOMEM;
    }
}

ugdr_qp *ugdr_create_qp(ugdr_pd *, ugdr_qp_init_attr *) noexcept {
    return unsupported_pointer<ugdr_qp>();
}

int ugdr_destroy_qp(ugdr_qp *) noexcept {
    return kUnsupported;
}

int ugdr_modify_qp(ugdr_qp *, ugdr_qp_attr *, int) noexcept {
    return kUnsupported;
}

int ugdr_query_qp(ugdr_qp *, ugdr_qp_attr *, int, ugdr_qp_init_attr *) noexcept {
    return kUnsupported;
}

int ugdr_query_qp_conn_info(ugdr_qp *, ugdr_qp_conn_info *) noexcept {
    return kUnsupported;
}

int ugdr_connect_qp(ugdr_qp *, const ugdr_qp_conn_info *, const ugdr_qp_attr *, int) noexcept {
    return kUnsupported;
}

int ugdr_post_send(ugdr_qp *, ugdr_send_wr *, ugdr_send_wr **) noexcept {
    return kUnsupported;
}

int ugdr_post_recv(ugdr_qp *, ugdr_recv_wr *, ugdr_recv_wr **) noexcept {
    return kUnsupported;
}

}  // extern "C"

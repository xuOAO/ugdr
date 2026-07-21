#include "ugdr/api.hpp"

#include "control/device_context.hpp"

#include <cerrno>

#include <cstdint>
#include <cstdlib>
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

namespace {

struct DeviceListRecord {
    std::unique_ptr<ugdr_device *[]> pointers;
    std::vector<ugdr_device *> devices;
    bool live = true;
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
    std::unordered_map<ugdr_device **, DeviceListRecord *> lists_;
    std::unordered_set<ugdr_device *> devices_;
    std::unordered_set<ugdr_context *> contexts_;
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

ugdr_pd *ugdr_alloc_pd(ugdr_context *) noexcept {
    return unsupported_pointer<ugdr_pd>();
}

int ugdr_dealloc_pd(ugdr_pd *) noexcept {
    return kUnsupported;
}

ugdr_mr *ugdr_reg_mr(ugdr_pd *, void *, size_t, int) noexcept {
    return unsupported_pointer<ugdr_mr>();
}

int ugdr_dereg_mr(ugdr_mr *) noexcept {
    return kUnsupported;
}

ugdr_cq *ugdr_create_cq(ugdr_context *, int, void *, ugdr_comp_channel *, int) noexcept {
    return unsupported_pointer<ugdr_cq>();
}

int ugdr_destroy_cq(ugdr_cq *) noexcept {
    return kUnsupported;
}

int ugdr_poll_cq(ugdr_cq *, int, ugdr_wc *) noexcept {
    return -kUnsupported;
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

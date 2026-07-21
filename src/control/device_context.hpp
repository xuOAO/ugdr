#pragma once

#include "control/control.hpp"
#include "control/object_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ugdr::control {

constexpr const char *kDefaultDaemonSocket = "/run/ugdr/ugdr.sock";

enum class ControlMethod : std::uint32_t {
    list_devices = 1,
    create_context = 2,
    destroy_context = 3,
    create_pd = 4,
    destroy_pd = 5,
    register_mr = 6,
    deregister_mr = 7,
    create_cq = 8,
    destroy_cq = 9,
    create_qp = 10,
    destroy_qp = 11,
    query_qp = 12,
    modify_qp = 13,
    query_qp_conn_info = 14,
    connect_qp = 15,
};

struct DeviceDescriptor {
    std::uint64_t identity = 0;
    std::string name;

    bool operator==(const DeviceDescriptor &) const = default;
};

UgdrControlRequest make_list_devices_request();
UgdrControlRequest make_create_context_request(std::uint64_t device_identity);
UgdrControlRequest make_destroy_context_request(std::uint64_t context_identity);
int encode_device_list(const std::vector<DeviceDescriptor> &devices, std::vector<std::byte> *bytes);
int decode_device_list(const std::vector<std::byte> &bytes, std::vector<DeviceDescriptor> *devices);

class DeviceCatalog {
  public:
    DeviceCatalog();
    explicit DeviceCatalog(std::vector<DeviceDescriptor> devices);

    [[nodiscard]] const std::vector<DeviceDescriptor> &devices() const noexcept;
    [[nodiscard]] bool contains(std::uint64_t identity) const noexcept;

  private:
    std::vector<DeviceDescriptor> devices_;
};

struct ContextRecord {
    std::uint64_t device_identity = 0;
    std::size_t child_count = 0;
};

class DeviceContextService : public ControlService {
  public:
    DeviceContextService();
    explicit DeviceContextService(DeviceCatalog catalog);

    ControlServiceResult handle(ipc::SessionId session_id, DecodedControlRequest request) override;
    void on_disconnect(ipc::SessionId session_id) noexcept override;

    [[nodiscard]] std::size_t context_count() const noexcept;

  protected:
    ContextRecord *resolve_context(ipc::SessionId session_id, std::uint64_t identity) noexcept;
    const ContextRecord *resolve_context(ipc::SessionId session_id,
                                         std::uint64_t identity) const noexcept;

  private:
    DeviceCatalog catalog_;
    GenerationRegistry<ContextRecord, ObjectType::context> contexts_;
};

class ControlClient {
  public:
    ControlClient();
    ~ControlClient();

    ControlClient(const ControlClient &) = delete;
    ControlClient &operator=(const ControlClient &) = delete;

    int connect(const std::string &socket_path);
    void close() noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] std::uint64_t connection_epoch() const noexcept;

    int list_devices(std::vector<DeviceDescriptor> *devices);
    int create_context(std::uint64_t device_identity, std::uint64_t *context_identity);
    int destroy_context(std::uint64_t context_identity);
    int call(UgdrControlRequest request, UgdrControlResponse *response);
    int call(UgdrControlRequest request, DecodedControlResponse *response);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ugdr::control

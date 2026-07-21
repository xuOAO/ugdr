#pragma once

#include "control/ipc_adapter.hpp"

namespace ugdr {

int control_placeholder() noexcept;

namespace control {

struct ControlServiceResult {
    UgdrControlResponse response;
    std::vector<ipc::UniqueFd> file_descriptors;
};

class ControlService {
  public:
    virtual ~ControlService() = default;
    virtual ControlServiceResult handle(ipc::SessionId session_id,
                                        DecodedControlRequest request) = 0;
    virtual void on_disconnect(ipc::SessionId session_id) noexcept;
};

class UnsupportedControlService final : public ControlService {
  public:
    ControlServiceResult handle(ipc::SessionId session_id, DecodedControlRequest request) override;
};

class ControlIpcHandler final : public ipc::IpcHandler {
  public:
    explicit ControlIpcHandler(ControlService &service) noexcept;

    ipc::IpcMessage handle(ipc::SessionId session_id, ipc::IpcMessage &&request) override;
    void on_disconnect(ipc::SessionId session_id) noexcept override;

  private:
    ControlService &service_;
};

}  // namespace control

}  // namespace ugdr

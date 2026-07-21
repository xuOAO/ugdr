#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ugdr::ipc {

constexpr std::uint32_t kProtocolMagic = UINT32_C(0x49504331);
constexpr std::uint16_t kProtocolMajor = 1;
constexpr std::uint16_t kProtocolMinor = 0;
constexpr std::uint32_t kResponseFlag = UINT32_C(1);
constexpr std::uint32_t kAllowedFlags = kResponseFlag;
constexpr std::size_t kWireHeaderSize = 36;
constexpr std::size_t kMaxPayloadSize = 1024 * 1024;
constexpr std::size_t kMaxFileDescriptors = 16;

class UniqueFd {
  public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int descriptor) noexcept;
    ~UniqueFd();

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;
    UniqueFd(UniqueFd &&other) noexcept;
    UniqueFd &operator=(UniqueFd &&other) noexcept;

    [[nodiscard]] int get() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    int release() noexcept;
    void reset(int descriptor = -1) noexcept;

  private:
    int descriptor_ = -1;
};

struct Envelope {
    std::uint32_t magic = kProtocolMagic;
    std::uint16_t version_major = kProtocolMajor;
    std::uint16_t version_minor = kProtocolMinor;
    std::uint32_t method = 0;
    std::uint32_t flags = 0;
    std::uint64_t request_id = 0;
    std::int32_t status = 0;
    std::uint32_t payload_length = 0;
    std::uint32_t fd_count = 0;
};

struct IpcMessage {
    Envelope envelope;
    std::vector<std::byte> payload;
    std::vector<UniqueFd> file_descriptors;
};

enum class ReceiveState {
    message,
    eof,
    error,
};

struct ReceiveResult {
    ReceiveState state = ReceiveState::error;
    IpcMessage message;
    int error_number = 0;
};

int send_message(int socket_fd, const IpcMessage &message);
ReceiveResult receive_message(int socket_fd);

class IpcClient {
  public:
    IpcClient() noexcept = default;
    ~IpcClient() = default;

    IpcClient(const IpcClient &) = delete;
    IpcClient &operator=(const IpcClient &) = delete;
    IpcClient(IpcClient &&) noexcept = default;
    IpcClient &operator=(IpcClient &&) noexcept = default;

    int connect(const std::string &socket_path);
    int call(std::uint32_t method, std::vector<std::byte> payload,
             std::vector<UniqueFd> file_descriptors, IpcMessage *response);
    void close() noexcept;
    [[nodiscard]] bool connected() const noexcept;

  private:
    UniqueFd socket_;
    std::uint64_t next_request_id_ = 1;
};

using SessionId = std::uint64_t;

class IpcHandler {
  public:
    virtual ~IpcHandler() = default;
    virtual IpcMessage handle(SessionId session_id, IpcMessage &&request) = 0;
    virtual void on_disconnect(SessionId session_id) noexcept;
};

class IpcServer {
  public:
    explicit IpcServer(IpcHandler &handler) noexcept;
    ~IpcServer();

    IpcServer(const IpcServer &) = delete;
    IpcServer &operator=(const IpcServer &) = delete;

    int start(const std::string &socket_path);
    int poll_once(int timeout_ms);
    void close() noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::size_t session_count() const noexcept;

  private:
    struct Connection {
        UniqueFd socket;
        SessionId session_id = 0;
    };

    int accept_ready_connections();
    void close_session(int socket_fd) noexcept;
    void remove_socket_path() noexcept;

    IpcHandler &handler_;
    UniqueFd listener_;
    std::unordered_map<int, Connection> connections_;
    std::string socket_path_;
    std::uint64_t socket_device_ = 0;
    std::uint64_t socket_inode_ = 0;
    SessionId next_session_id_ = 1;
    bool owns_socket_path_ = false;
};

}  // namespace ugdr::ipc

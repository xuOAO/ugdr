#include "ipc/ipc.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <limits>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <utility>

namespace ugdr::ipc {
namespace {

std::uint64_t host_to_network64(std::uint64_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (static_cast<std::uint64_t>(htonl(static_cast<std::uint32_t>(value))) << 32U) |
           htonl(static_cast<std::uint32_t>(value >> 32U));
#else
    return value;
#endif
}

std::uint64_t network_to_host64(std::uint64_t value) noexcept {
    return host_to_network64(value);
}

template <typename T>
void append_scalar(std::array<std::byte, kWireHeaderSize> &buffer, std::size_t *offset, T value) {
    std::memcpy(buffer.data() + *offset, &value, sizeof(value));
    *offset += sizeof(value);
}

template <typename T> T read_scalar(const std::byte *buffer, std::size_t *offset) {
    T value{};
    std::memcpy(&value, buffer + *offset, sizeof(value));
    *offset += sizeof(value);
    return value;
}

std::array<std::byte, kWireHeaderSize> encode_header(const Envelope &envelope) {
    std::array<std::byte, kWireHeaderSize> buffer{};
    std::size_t offset = 0;
    append_scalar(buffer, &offset, htonl(envelope.magic));
    append_scalar(buffer, &offset, htons(envelope.version_major));
    append_scalar(buffer, &offset, htons(envelope.version_minor));
    append_scalar(buffer, &offset, htonl(envelope.method));
    append_scalar(buffer, &offset, htonl(envelope.flags));
    append_scalar(buffer, &offset, host_to_network64(envelope.request_id));
    append_scalar(buffer, &offset, htonl(static_cast<std::uint32_t>(envelope.status)));
    append_scalar(buffer, &offset, htonl(envelope.payload_length));
    append_scalar(buffer, &offset, htonl(envelope.fd_count));
    return buffer;
}

Envelope decode_header(const std::byte *buffer) {
    Envelope envelope;
    std::size_t offset = 0;
    envelope.magic = ntohl(read_scalar<std::uint32_t>(buffer, &offset));
    envelope.version_major = ntohs(read_scalar<std::uint16_t>(buffer, &offset));
    envelope.version_minor = ntohs(read_scalar<std::uint16_t>(buffer, &offset));
    envelope.method = ntohl(read_scalar<std::uint32_t>(buffer, &offset));
    envelope.flags = ntohl(read_scalar<std::uint32_t>(buffer, &offset));
    envelope.request_id = network_to_host64(read_scalar<std::uint64_t>(buffer, &offset));
    envelope.status = static_cast<std::int32_t>(ntohl(read_scalar<std::uint32_t>(buffer, &offset)));
    envelope.payload_length = ntohl(read_scalar<std::uint32_t>(buffer, &offset));
    envelope.fd_count = ntohl(read_scalar<std::uint32_t>(buffer, &offset));
    return envelope;
}

ReceiveResult receive_error(int error_number) {
    ReceiveResult result;
    result.state = ReceiveState::error;
    result.error_number = error_number;
    return result;
}

}  // namespace

UniqueFd::UniqueFd(int descriptor) noexcept : descriptor_(descriptor) {
}

UniqueFd::~UniqueFd() {
    reset();
}

UniqueFd::UniqueFd(UniqueFd &&other) noexcept : descriptor_(other.release()) {
}

UniqueFd &UniqueFd::operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int UniqueFd::get() const noexcept {
    return descriptor_;
}

bool UniqueFd::valid() const noexcept {
    return descriptor_ >= 0;
}

int UniqueFd::release() noexcept {
    const int descriptor = descriptor_;
    descriptor_ = -1;
    return descriptor;
}

void UniqueFd::reset(int descriptor) noexcept {
    if (descriptor_ >= 0) {
        while (::close(descriptor_) < 0 && errno == EINTR) {
        }
    }
    descriptor_ = descriptor;
}

int send_message(int socket_fd, const IpcMessage &message) {
    if (socket_fd < 0 || message.payload.size() > kMaxPayloadSize ||
        message.file_descriptors.size() > kMaxFileDescriptors ||
        message.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return -EINVAL;
    }
    for (const auto &descriptor : message.file_descriptors) {
        if (!descriptor.valid()) {
            return -EBADF;
        }
    }

    Envelope envelope = message.envelope;
    envelope.payload_length = static_cast<std::uint32_t>(message.payload.size());
    envelope.fd_count = static_cast<std::uint32_t>(message.file_descriptors.size());
    const auto header = encode_header(envelope);

    std::array<iovec, 2> vectors{};
    vectors[0].iov_base = const_cast<std::byte *>(header.data());
    vectors[0].iov_len = header.size();
    vectors[1].iov_base = const_cast<std::byte *>(message.payload.data());
    vectors[1].iov_len = message.payload.size();

    std::vector<std::byte> control;
    msghdr message_header{};
    message_header.msg_iov = vectors.data();
    message_header.msg_iovlen = message.payload.empty() ? 1U : vectors.size();
    if (!message.file_descriptors.empty()) {
        const std::size_t descriptor_bytes = message.file_descriptors.size() * sizeof(int);
        control.resize(CMSG_SPACE(descriptor_bytes));
        message_header.msg_control = control.data();
        message_header.msg_controllen = control.size();
        cmsghdr *const control_header = CMSG_FIRSTHDR(&message_header);
        control_header->cmsg_level = SOL_SOCKET;
        control_header->cmsg_type = SCM_RIGHTS;
        control_header->cmsg_len = CMSG_LEN(descriptor_bytes);
        auto *const descriptor_data = reinterpret_cast<int *>(CMSG_DATA(control_header));
        for (std::size_t index = 0; index < message.file_descriptors.size(); ++index) {
            descriptor_data[index] = message.file_descriptors[index].get();
        }
    }

    const std::size_t expected_size = header.size() + message.payload.size();
    const ssize_t sent = ::sendmsg(socket_fd, &message_header, MSG_NOSIGNAL);
    if (sent < 0) {
        return -errno;
    }
    return static_cast<std::size_t>(sent) == expected_size ? 0 : -EIO;
}

ReceiveResult receive_message(int socket_fd) {
    if (socket_fd < 0) {
        return receive_error(EBADF);
    }

    std::vector<std::byte> bytes(kWireHeaderSize + kMaxPayloadSize);
    std::vector<std::byte> control(CMSG_SPACE(kMaxFileDescriptors * sizeof(int)));
    iovec vector{bytes.data(), bytes.size()};
    msghdr message_header{};
    message_header.msg_iov = &vector;
    message_header.msg_iovlen = 1;
    message_header.msg_control = control.data();
    message_header.msg_controllen = control.size();

    const ssize_t received = ::recvmsg(socket_fd, &message_header, MSG_CMSG_CLOEXEC);
    if (received == 0) {
        ReceiveResult result;
        result.state = ReceiveState::eof;
        return result;
    }
    if (received < 0) {
        return receive_error(errno);
    }

    std::vector<UniqueFd> descriptors;
    bool invalid_control = (message_header.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0;
    for (cmsghdr *header = CMSG_FIRSTHDR(&message_header); header != nullptr;
         header = CMSG_NXTHDR(&message_header, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len < CMSG_LEN(0)) {
            invalid_control = true;
            continue;
        }
        const std::size_t data_size = header->cmsg_len - CMSG_LEN(0);
        const std::size_t count = data_size / sizeof(int);
        if (data_size % sizeof(int) != 0 || descriptors.size() + count > kMaxFileDescriptors) {
            invalid_control = true;
        }
        const auto *const values = reinterpret_cast<const int *>(CMSG_DATA(header));
        for (std::size_t index = 0; index < count; ++index) {
            descriptors.emplace_back(values[index]);
        }
    }
    if (invalid_control || static_cast<std::size_t>(received) < kWireHeaderSize) {
        return receive_error(EMSGSIZE);
    }

    Envelope envelope = decode_header(bytes.data());
    const std::size_t payload_size = static_cast<std::size_t>(received) - kWireHeaderSize;
    if (envelope.magic != kProtocolMagic || envelope.version_major != kProtocolMajor ||
        envelope.version_minor > kProtocolMinor) {
        return receive_error(EPROTONOSUPPORT);
    }
    if ((envelope.flags & ~kAllowedFlags) != 0 || envelope.payload_length != payload_size ||
        envelope.fd_count != descriptors.size()) {
        return receive_error(EPROTO);
    }

    ReceiveResult result;
    result.state = ReceiveState::message;
    result.message.envelope = envelope;
    result.message.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kWireHeaderSize),
                                  bytes.begin() + received);
    result.message.file_descriptors = std::move(descriptors);
    return result;
}

void IpcHandler::on_disconnect(SessionId) noexcept {
}

}  // namespace ugdr::ipc

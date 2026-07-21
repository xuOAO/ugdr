#include "ipc/ipc.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <iostream>
#include <vector>

namespace {

std::vector<std::byte> payload(std::initializer_list<unsigned char> values) {
    std::vector<std::byte> bytes;
    bytes.reserve(values.size());
    for (const unsigned char value : values) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

bool pipe_pair(std::array<int, 2> *descriptors) {
    return ::pipe2(descriptors->data(), O_CLOEXEC) == 0;
}

}  // namespace

int main() {
    std::array<int, 2> sockets{};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets.data()) != 0) {
        return 1;
    }
    ugdr::ipc::UniqueFd sender(sockets[0]);
    ugdr::ipc::UniqueFd receiver(sockets[1]);

    ugdr::ipc::IpcMessage empty_fd_message;
    empty_fd_message.envelope.method = 7;
    empty_fd_message.envelope.request_id = 11;
    empty_fd_message.payload = payload({1, 2, 3});
    if (ugdr::ipc::send_message(sender.get(), empty_fd_message) != 0) {
        return 2;
    }
    auto received = ugdr::ipc::receive_message(receiver.get());
    if (received.state != ugdr::ipc::ReceiveState::message ||
        received.message.envelope.method != 7 || received.message.envelope.request_id != 11 ||
        received.message.payload != empty_fd_message.payload ||
        !received.message.file_descriptors.empty()) {
        return 3;
    }

    std::array<int, 2> first_pipe{};
    std::array<int, 2> second_pipe{};
    if (!pipe_pair(&first_pipe) || !pipe_pair(&second_pipe)) {
        return 4;
    }
    ugdr::ipc::UniqueFd first_read(first_pipe[0]);
    ugdr::ipc::UniqueFd first_write(first_pipe[1]);
    ugdr::ipc::UniqueFd second_read(second_pipe[0]);
    ugdr::ipc::UniqueFd second_write(second_pipe[1]);

    ugdr::ipc::IpcMessage fd_message;
    fd_message.envelope.method = 13;
    fd_message.file_descriptors.emplace_back(::dup(first_read.get()));
    fd_message.file_descriptors.emplace_back(::dup(second_read.get()));
    if (!fd_message.file_descriptors[0].valid() || !fd_message.file_descriptors[1].valid() ||
        ugdr::ipc::send_message(sender.get(), fd_message) != 0) {
        return 5;
    }
    received = ugdr::ipc::receive_message(receiver.get());
    if (received.state != ugdr::ipc::ReceiveState::message ||
        received.message.file_descriptors.size() != 2 || ::fcntl(first_read.get(), F_GETFD) < 0 ||
        ::fcntl(second_read.get(), F_GETFD) < 0) {
        return 6;
    }
    const char first_value = 'a';
    const char second_value = 'b';
    if (::write(first_write.get(), &first_value, 1) != 1 ||
        ::write(second_write.get(), &second_value, 1) != 1) {
        return 7;
    }
    char actual_first = 0;
    char actual_second = 0;
    if (::read(received.message.file_descriptors[0].get(), &actual_first, 1) != 1 ||
        ::read(received.message.file_descriptors[1].get(), &actual_second, 1) != 1 ||
        actual_first != first_value || actual_second != second_value) {
        return 8;
    }

    ugdr::ipc::IpcMessage invalid_flags;
    invalid_flags.envelope.method = 17;
    invalid_flags.envelope.flags = UINT32_C(0x80);
    if (ugdr::ipc::send_message(sender.get(), invalid_flags) != 0) {
        return 9;
    }
    received = ugdr::ipc::receive_message(receiver.get());
    if (received.state != ugdr::ipc::ReceiveState::error || received.error_number != EPROTO) {
        return 10;
    }

    const std::array<std::byte, 3> truncated{};
    iovec truncated_vector{const_cast<std::byte *>(truncated.data()), truncated.size()};
    msghdr truncated_message{};
    truncated_message.msg_iov = &truncated_vector;
    truncated_message.msg_iovlen = 1;
    const ssize_t truncated_status = ::sendmsg(sender.get(), &truncated_message, MSG_NOSIGNAL);
    if (truncated_status != static_cast<ssize_t>(truncated.size())) {
        std::cerr << "truncated send failed: result=" << truncated_status << " errno=" << errno
                  << '\n';
        return 11;
    }
    received = ugdr::ipc::receive_message(receiver.get());
    return received.state == ugdr::ipc::ReceiveState::error && received.error_number == EMSGSIZE
               ? 0
               : 12;
}

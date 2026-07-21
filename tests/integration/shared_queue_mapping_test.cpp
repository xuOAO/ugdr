#include "ipc/ipc.hpp"
#include "queue/shared_ring.hpp"

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

int wait_consumer(ugdr::queue::SharedRing *ring, std::uint64_t expected) {
    for (int attempt = 0; attempt < 100000; ++attempt) {
        const void *slot = nullptr;
        const int status = ring->consumer_peek(&slot);
        if (status == EAGAIN) {
            std::this_thread::yield();
            continue;
        }
        std::uint64_t actual = 0;
        if (status != 0) {
            return status;
        }
        std::memcpy(&actual, slot, sizeof(actual));
        return actual == expected ? ring->consumer_release() : EPROTO;
    }
    return ETIMEDOUT;
}

int publish(ugdr::queue::SharedRing *ring, std::uint64_t value) {
    void *slot = nullptr;
    if (ring->producer_reserve(&slot) != 0) {
        return EAGAIN;
    }
    std::memcpy(slot, &value, sizeof(value));
    return ring->producer_publish();
}

int child_main(int socket_fd, const std::array<ugdr::queue::QueueDescriptor, 3> &descriptors) {
    ugdr::ipc::ReceiveResult received = ugdr::ipc::receive_message(socket_fd);
    if (received.state != ugdr::ipc::ReceiveState::message ||
        received.message.file_descriptors.size() != descriptors.size()) {
        return 20;
    }
    ugdr::queue::SharedRing sq;
    ugdr::queue::SharedRing rq;
    ugdr::queue::SharedRing cq;
    if (ugdr::queue::map_shared_ring(received.message.file_descriptors[0].get(), descriptors[0],
                                     &sq) != 0 ||
        ugdr::queue::map_shared_ring(received.message.file_descriptors[1].get(), descriptors[1],
                                     &rq) != 0 ||
        ugdr::queue::map_shared_ring(received.message.file_descriptors[2].get(), descriptors[2],
                                     &cq) != 0) {
        return 21;
    }
    if (publish(&sq, 0x51) != 0 || publish(&rq, 0x52) != 0 || wait_consumer(&cq, 0x43) != 0) {
        return 22;
    }
    return 0;
}

}  // namespace

int main() {
    const std::array descriptors{
        ugdr::queue::QueueDescriptor{ugdr::queue::QueueKind::send, 5, 64},
        ugdr::queue::QueueDescriptor{ugdr::queue::QueueKind::receive, 3, 64},
        ugdr::queue::QueueDescriptor{ugdr::queue::QueueKind::completion, 7, 64}};
    std::array<ugdr::queue::SharedRing, 3> rings;
    for (std::size_t index = 0; index < rings.size(); ++index) {
        if (ugdr::queue::create_shared_ring(descriptors[index], &rings[index]) != 0) {
            return 1;
        }
    }
    std::array<int, 2> sockets{};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets.data()) != 0) {
        return 2;
    }
    const pid_t child = fork();
    if (child < 0) {
        return 3;
    }
    if (child == 0) {
        (void)::close(sockets[0]);
        const int result = child_main(sockets[1], descriptors);
        (void)::close(sockets[1]);
        _exit(result);
    }
    (void)::close(sockets[1]);
    ugdr::ipc::IpcMessage message;
    message.envelope.method = 1;
    for (ugdr::queue::SharedRing &ring : rings) {
        int fd = -1;
        if (ring.duplicate_fd(&fd) != 0) {
            return 4;
        }
        message.file_descriptors.emplace_back(fd);
    }
    if (ugdr::ipc::send_message(sockets[0], message) != 0 || wait_consumer(&rings[0], 0x51) != 0 ||
        wait_consumer(&rings[1], 0x52) != 0 || publish(&rings[2], 0x43) != 0) {
        return 5;
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return 6;
    }
    return 0;
}

#include "queue/shared_ring.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <thread>

#include <unistd.h>

namespace {

int basic_capacity_test(std::uint32_t capacity) {
    const ugdr::queue::QueueDescriptor descriptor{ugdr::queue::QueueKind::send, capacity, 64};
    ugdr::queue::SharedRing ring;
    if (ugdr::queue::create_shared_ring(descriptor, &ring) != 0 ||
        ring.descriptor() != descriptor || ring.mapping_size() == 0) {
        return 1;
    }
    for (std::uint32_t index = 0; index < capacity; ++index) {
        void *slot = nullptr;
        if (ring.producer_reserve(&slot) != 0) {
            return 2;
        }
        std::memcpy(slot, &index, sizeof(index));
        if (ring.producer_publish() != 0) {
            return 3;
        }
    }
    void *full = nullptr;
    if (ring.producer_reserve(&full) != EAGAIN) {
        return 4;
    }
    for (std::uint32_t index = 0; index < capacity; ++index) {
        const void *slot = nullptr;
        std::uint32_t actual = UINT32_MAX;
        if (ring.consumer_peek(&slot) != 0) {
            return 5;
        }
        std::memcpy(&actual, slot, sizeof(actual));
        if (actual != index || ring.consumer_release() != 0) {
            return 6;
        }
    }
    const void *empty = nullptr;
    return ring.consumer_peek(&empty) == EAGAIN ? 0 : 7;
}

int mapping_test() {
    const ugdr::queue::QueueDescriptor descriptor{ugdr::queue::QueueKind::receive, 3, 64};
    ugdr::queue::SharedRing owner;
    if (ugdr::queue::create_shared_ring(descriptor, &owner) != 0) {
        return 1;
    }
    int fd = -1;
    if (owner.duplicate_fd(&fd) != 0) {
        return 2;
    }
    ugdr::queue::SharedRing peer;
    const int map_status = ugdr::queue::map_shared_ring(fd, descriptor, &peer);
    (void)::close(fd);
    if (map_status != 0 || owner.mapping_address() == peer.mapping_address()) {
        return 3;
    }
    void *slot = nullptr;
    std::uint64_t expected = UINT64_C(0x1122334455667788);
    if (owner.producer_reserve(&slot) != 0) {
        return 4;
    }
    std::memcpy(slot, &expected, sizeof(expected));
    if (owner.producer_publish() != 0) {
        return 5;
    }
    const void *read_slot = nullptr;
    std::uint64_t actual = 0;
    if (peer.consumer_peek(&read_slot) != 0) {
        return 6;
    }
    std::memcpy(&actual, read_slot, sizeof(actual));
    return actual == expected && peer.consumer_release() == 0 ? 0 : 7;
}

int threaded_wrap_test() {
    constexpr std::uint64_t iterations = 200000;
    const ugdr::queue::QueueDescriptor descriptor{ugdr::queue::QueueKind::completion, 7, 64};
    ugdr::queue::SharedRing ring;
    if (ugdr::queue::create_shared_ring(descriptor, &ring) != 0) {
        return 1;
    }
    std::atomic<int> error{0};
    std::thread producer([&] {
        for (std::uint64_t value = 0; value < iterations && error.load() == 0; ++value) {
            void *slot = nullptr;
            while (ring.producer_reserve(&slot) == EAGAIN) {
                std::this_thread::yield();
            }
            std::memcpy(slot, &value, sizeof(value));
            if (ring.producer_publish() != 0) {
                error = 2;
            }
        }
    });
    std::thread consumer([&] {
        for (std::uint64_t expected = 0; expected < iterations && error.load() == 0; ++expected) {
            const void *slot = nullptr;
            while (ring.consumer_peek(&slot) == EAGAIN) {
                std::this_thread::yield();
            }
            std::uint64_t actual = 0;
            std::memcpy(&actual, slot, sizeof(actual));
            if (actual != expected || ring.consumer_release() != 0) {
                error = 3;
            }
        }
    });
    producer.join();
    consumer.join();
    return error.load();
}

int malformed_mapping_test() {
    const ugdr::queue::QueueDescriptor descriptor{ugdr::queue::QueueKind::send, 2, 64};
    ugdr::queue::SharedRing owner;
    if (ugdr::queue::create_shared_ring(descriptor, &owner) != 0) {
        return 1;
    }
    int fd = -1;
    if (owner.duplicate_fd(&fd) != 0) {
        return 2;
    }
    auto *header =
        static_cast<ugdr::queue::SharedRingHeader *>(const_cast<void *>(owner.mapping_address()));
    header->metadata.version = ugdr::queue::kSharedRingVersion + 1;
    ugdr::queue::SharedRing rejected;
    if (ugdr::queue::map_shared_ring(fd, descriptor, &rejected) != EPROTONOSUPPORT) {
        (void)::close(fd);
        return 3;
    }
    header->metadata.version = ugdr::queue::kSharedRingVersion;
    header->metadata.kind = static_cast<std::uint16_t>(ugdr::queue::QueueKind::receive);
    if (ugdr::queue::map_shared_ring(fd, descriptor, &rejected) != EPROTO) {
        (void)::close(fd);
        return 4;
    }
    (void)::close(fd);
    return 0;
}

}  // namespace

int main() {
    if (basic_capacity_test(1) != 0) {
        return 1;
    }
    if (basic_capacity_test(3) != 0) {
        return 2;
    }
    if (mapping_test() != 0) {
        return 3;
    }
    if (threaded_wrap_test() != 0) {
        return 4;
    }
    if (malformed_mapping_test() != 0) {
        return 5;
    }
    std::size_t ignored = 0;
    const ugdr::queue::QueueDescriptor invalid{ugdr::queue::QueueKind::send, 1, 63};
    return ugdr::queue::shared_ring_mapping_size(invalid, 4096, &ignored) == EINVAL ? 0 : 6;
}

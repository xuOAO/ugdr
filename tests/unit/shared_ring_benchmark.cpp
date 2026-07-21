#include "queue/shared_ring.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <thread>

#include <unistd.h>

namespace {

constexpr std::uint32_t kCapacity = 4096;
constexpr std::uint32_t kStride = 64;
constexpr std::uint64_t kIterations = 10'000'000;

void write_span(const ugdr::queue::MutableSlotSpan &span, std::uint64_t *value) {
    auto *bytes = static_cast<std::byte *>(span.data);
    for (std::uint32_t index = 0; index < span.count; ++index) {
        std::memcpy(bytes + static_cast<std::size_t>(index) * kStride, value, sizeof(*value));
        ++*value;
    }
}

bool read_span(const ugdr::queue::ConstSlotSpan &span, std::uint64_t *expected) {
    const auto *bytes = static_cast<const std::byte *>(span.data);
    for (std::uint32_t index = 0; index < span.count; ++index) {
        std::uint64_t actual = 0;
        std::memcpy(&actual, bytes + static_cast<std::size_t>(index) * kStride, sizeof(actual));
        if (actual != *expected) {
            return false;
        }
        ++*expected;
    }
    return true;
}

int run(std::uint32_t batch_size) {
    const ugdr::queue::QueueDescriptor descriptor{ugdr::queue::QueueKind::send, kCapacity, kStride};
    ugdr::queue::SharedRing producer;
    if (ugdr::queue::create_shared_ring(descriptor, &producer) != 0) {
        return 1;
    }
    int descriptor_fd = -1;
    if (producer.duplicate_fd(&descriptor_fd) != 0) {
        return 2;
    }
    ugdr::queue::SharedRing consumer;
    const int map_status = ugdr::queue::map_shared_ring(descriptor_fd, descriptor, &consumer);
    (void)::close(descriptor_fd);
    if (map_status != 0) {
        return 3;
    }

    std::atomic<bool> failed{false};
    const auto begin = std::chrono::steady_clock::now();
    std::thread producer_thread([&] {
        std::uint64_t value = 0;
        while (value < kIterations && !failed.load(std::memory_order_relaxed)) {
            ugdr::queue::MutableSlotBatch batch;
            const auto requested = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(batch_size, kIterations - value));
            const int status = producer.producer_reserve(requested, &batch);
            if (status == EAGAIN) {
                continue;
            }
            if (status != 0) {
                failed.store(true, std::memory_order_relaxed);
                break;
            }
            write_span(batch.first, &value);
            write_span(batch.second, &value);
            if (producer.producer_publish(batch.count) != 0) {
                failed.store(true, std::memory_order_relaxed);
            }
        }
    });
    std::uint64_t expected = 0;
    while (expected < kIterations && !failed.load(std::memory_order_relaxed)) {
        ugdr::queue::ConstSlotBatch batch;
        const auto requested =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(batch_size, kIterations - expected));
        const int status = consumer.consumer_peek(requested, &batch);
        if (status == EAGAIN) {
            continue;
        }
        if (status != 0 || !read_span(batch.first, &expected) ||
            !read_span(batch.second, &expected) || consumer.consumer_release(batch.count) != 0) {
            failed.store(true, std::memory_order_relaxed);
        }
    }
    producer_thread.join();
    const auto end = std::chrono::steady_clock::now();
    if (failed.load(std::memory_order_relaxed) || expected != kIterations) {
        return 4;
    }
    const double seconds = std::chrono::duration<double>(end - begin).count();
    const double descriptors_per_second = static_cast<double>(kIterations) / seconds;
    const double minimum_payload = 50'000'000'000.0 / descriptors_per_second;
    std::cout << "batch=" << batch_size << " descriptors_per_second=" << std::fixed
              << std::setprecision(0) << descriptors_per_second
              << " minimum_bytes_for_400Gbps=" << std::setprecision(1) << minimum_payload << '\n';
    return 0;
}

}  // namespace

int main() {
    if (run(1) != 0) {
        return 1;
    }
    return run(32);
}

#include "api/wr_posting.hpp"
#include "queue/descriptors.hpp"
#include "queue/shared_ring.hpp"

#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

constexpr std::uint32_t kCapacity = 64;
constexpr std::uint64_t kDescriptorCount = UINT64_C(4000000);

bool create_pair(ugdr::queue::SharedRing *producer, ugdr::queue::SharedRing *consumer) {
    std::uint32_t stride = 0;
    if (ugdr::queue::send_slot_stride(1, &stride) != 0) {
        return false;
    }
    const ugdr::queue::QueueDescriptor descriptor{ugdr::queue::QueueKind::send, kCapacity, stride};
    if (ugdr::queue::create_shared_ring(descriptor, producer) != 0) {
        return false;
    }
    int fd = -1;
    if (producer->duplicate_fd(&fd) != 0) {
        return false;
    }
    if (ugdr::queue::map_shared_ring(fd, descriptor, consumer) != 0) {
        ::close(fd);
        return false;
    }
    return true;
}

bool run(std::uint32_t batch_size) {
    ugdr::queue::SharedRing producer;
    ugdr::queue::SharedRing consumer;
    if (!create_pair(&producer, &consumer)) {
        return false;
    }
    std::array<ugdr_send_wr, 32> requests{};
    for (std::uint32_t index = 0; index < batch_size; ++index) {
        requests[index].wr_id = index;
        requests[index].opcode = UGDR_WR_RDMA_WRITE;
        requests[index].next = index + 1 < batch_size ? &requests[index + 1] : nullptr;
    }
    const std::uint64_t iterations = kDescriptorCount / batch_size;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        ugdr_send_wr *bad = nullptr;
        if (ugdr::api::post_send_chain(producer, 1, requests.data(), &bad) != 0 || bad != nullptr) {
            return false;
        }
        ugdr::queue::ConstSlotBatch slots;
        if (consumer.consumer_peek(batch_size, &slots) != 0 || slots.count != batch_size ||
            consumer.consumer_release(batch_size) != 0) {
            return false;
        }
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    const double descriptors_per_second =
        static_cast<double>(iterations * batch_size) / elapsed.count();
    std::printf(
        "benchmark=wr_posting build_type=%s cpu_threads=%u batch=%u iterations=%llu "
        "completed_wr=%llu MWR_per_s=%.3f descriptors_per_second=%.0f\n",
        UGDR_BENCHMARK_BUILD_TYPE, std::thread::hardware_concurrency(), batch_size,
        static_cast<unsigned long long>(iterations),
        static_cast<unsigned long long>(iterations * batch_size),
        descriptors_per_second / 1'000'000.0, descriptors_per_second);
    return true;
}

}  // namespace

int main() {
    return run(1) && run(32) ? 0 : 1;
}

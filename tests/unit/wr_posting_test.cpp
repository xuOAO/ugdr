#include "api/wr_posting.hpp"
#include "queue/descriptors.hpp"
#include "queue/shared_ring.hpp"

#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace {

std::atomic<std::size_t> allocation_count{};

struct RingPair {
    ugdr::queue::SharedRing producer;
    ugdr::queue::SharedRing consumer;
};

bool make_pair(ugdr::queue::QueueKind kind, std::uint32_t capacity, std::uint32_t max_sge,
               RingPair *pair) {
    std::uint32_t stride = 0;
    const int stride_status = kind == ugdr::queue::QueueKind::send
                                  ? ugdr::queue::send_slot_stride(max_sge, &stride)
                                  : ugdr::queue::receive_slot_stride(max_sge, &stride);
    if (stride_status != 0) {
        return false;
    }
    const ugdr::queue::QueueDescriptor descriptor{kind, capacity, stride};
    if (ugdr::queue::create_shared_ring(descriptor, &pair->producer) != 0) {
        return false;
    }
    int fd = -1;
    if (pair->producer.duplicate_fd(&fd) != 0) {
        return false;
    }
    const int map_status = ugdr::queue::map_shared_ring(fd, descriptor, &pair->consumer);
    if (map_status != 0) {
        ::close(fd);
        return false;
    }
    return true;
}

const std::byte *slot_at(const ugdr::queue::ConstSlotBatch &batch, std::uint32_t index,
                         std::uint32_t stride) {
    if (index < batch.first.count) {
        return static_cast<const std::byte *>(batch.first.data) +
               static_cast<std::size_t>(index) * stride;
    }
    return static_cast<const std::byte *>(batch.second.data) +
           static_cast<std::size_t>(index - batch.first.count) * stride;
}

bool send_copy_and_no_allocation() {
    RingPair pair;
    if (!make_pair(ugdr::queue::QueueKind::send, 4, 2, &pair)) {
        return false;
    }
    ugdr_sge first_sges[] = {{UINT64_C(0x1000), 64, 7}, {UINT64_C(0x2000), 32, 9}};
    ugdr_sge second_sge{UINT64_C(0x3000), 16, 11};
    ugdr_send_wr second{};
    second.wr_id = 102;
    second.sg_list = &second_sge;
    second.num_sge = 1;
    second.opcode = UGDR_WR_RDMA_WRITE_WITH_IMM;
    second.send_flags = UGDR_SEND_SIGNALED;
    second.imm_data = UINT32_C(0xaabbccdd);
    second.wr.rdma.remote_addr = UINT64_C(0x5000);
    second.wr.rdma.rkey = 17;
    ugdr_send_wr first{};
    first.wr_id = 101;
    first.next = &second;
    first.sg_list = first_sges;
    first.num_sge = 2;
    first.opcode = UGDR_WR_RDMA_WRITE;
    first.imm_data = UINT32_C(0xffffffff);
    first.wr.rdma.remote_addr = UINT64_C(0x4000);
    first.wr.rdma.rkey = 13;
    auto *const unchanged = reinterpret_cast<ugdr_send_wr *>(static_cast<std::uintptr_t>(1));
    auto *bad = unchanged;

    const std::size_t allocations_before = allocation_count.load(std::memory_order_relaxed);
    const int status = ugdr::api::post_send_chain(pair.producer, 2, &first, &bad);
    const std::size_t allocations_after = allocation_count.load(std::memory_order_relaxed);
    first_sges[0] = {};
    second_sge = {};

    ugdr::queue::ConstSlotBatch batch;
    if (status != 0 || bad != unchanged || allocations_before != allocations_after ||
        pair.consumer.consumer_peek(4, &batch) != 0 || batch.count != 2) {
        return false;
    }
    const std::uint32_t stride = pair.consumer.descriptor().slot_stride;
    const auto *first_header =
        reinterpret_cast<const ugdr::queue::SendWqeHeader *>(slot_at(batch, 0, stride));
    const auto *first_copy = reinterpret_cast<const ugdr::queue::SharedSge *>(first_header + 1);
    const auto *second_header =
        reinterpret_cast<const ugdr::queue::SendWqeHeader *>(slot_at(batch, 1, stride));
    const auto *second_copy = reinterpret_cast<const ugdr::queue::SharedSge *>(second_header + 1);
    const bool valid = first_header->wr_id == 101 &&
                       first_header->remote_address == UINT64_C(0x4000) &&
                       first_header->rkey == 13 && first_header->opcode == UGDR_WR_RDMA_WRITE &&
                       first_header->immediate_data == 0 && first_header->sge_count == 2 &&
                       first_header->reserved == 0 && first_copy[0].address == UINT64_C(0x1000) &&
                       first_copy[0].length == 64 && first_copy[0].lkey == 7 &&
                       first_copy[1].address == UINT64_C(0x2000) && first_copy[1].length == 32 &&
                       first_copy[1].lkey == 9 && second_header->wr_id == 102 &&
                       second_header->opcode == UGDR_WR_RDMA_WRITE_WITH_IMM &&
                       second_header->send_flags == UGDR_SEND_SIGNALED &&
                       second_header->immediate_data == UINT32_C(0xaabbccdd) &&
                       second_copy[0].address == UINT64_C(0x3000) && second_copy[0].lkey == 11;
    return valid && pair.consumer.consumer_release(2) == 0;
}

bool receive_copy_and_zero_sge() {
    RingPair pair;
    if (!make_pair(ugdr::queue::QueueKind::receive, 3, 2, &pair)) {
        return false;
    }
    ugdr_sge sges[] = {{UINT64_C(0x6000), 48, 19}, {UINT64_C(0x7000), 24, 23}};
    ugdr_recv_wr second{202, nullptr, sges, 2};
    ugdr_recv_wr first{201, &second, nullptr, 0};
    auto *const unchanged = reinterpret_cast<ugdr_recv_wr *>(static_cast<std::uintptr_t>(2));
    auto *bad = unchanged;
    if (ugdr::api::post_receive_chain(pair.producer, 2, &first, &bad) != 0 || bad != unchanged) {
        return false;
    }
    sges[0] = {};
    ugdr::queue::ConstSlotBatch batch;
    if (pair.consumer.consumer_peek(3, &batch) != 0 || batch.count != 2) {
        return false;
    }
    const std::uint32_t stride = pair.consumer.descriptor().slot_stride;
    const auto *first_header =
        reinterpret_cast<const ugdr::queue::ReceiveWqeHeader *>(slot_at(batch, 0, stride));
    const auto *second_header =
        reinterpret_cast<const ugdr::queue::ReceiveWqeHeader *>(slot_at(batch, 1, stride));
    const auto *copy = reinterpret_cast<const ugdr::queue::SharedSge *>(second_header + 1);
    return first_header->wr_id == 201 && first_header->sge_count == 0 &&
           first_header->reserved == 0 && second_header->wr_id == 202 &&
           second_header->sge_count == 2 && copy[0].address == UINT64_C(0x6000) &&
           copy[0].length == 48 && copy[0].lkey == 19 && copy[1].address == UINT64_C(0x7000) &&
           copy[1].length == 24 && copy[1].lkey == 23 && pair.consumer.consumer_release(2) == 0;
}

bool prefix_failure_full_and_wrap() {
    RingPair pair;
    if (!make_pair(ugdr::queue::QueueKind::send, 3, 1, &pair)) {
        return false;
    }
    ugdr_send_wr invalid{};
    invalid.wr_id = 302;
    invalid.num_sge = 2;
    invalid.opcode = UGDR_WR_RDMA_WRITE;
    ugdr_send_wr first{};
    first.wr_id = 301;
    first.next = &invalid;
    first.opcode = UGDR_WR_RDMA_WRITE;
    ugdr_send_wr *bad = nullptr;
    if (ugdr::api::post_send_chain(pair.producer, 1, &first, &bad) != EINVAL || bad != &invalid) {
        return false;
    }
    ugdr::queue::ConstSlotBatch prefix;
    if (pair.consumer.consumer_peek(3, &prefix) != 0 || prefix.count != 1 ||
        reinterpret_cast<const ugdr::queue::SendWqeHeader *>(prefix.first.data)->wr_id != 301 ||
        pair.consumer.consumer_release(1) != 0) {
        return false;
    }

    ugdr_send_wr third{};
    third.wr_id = 305;
    third.opcode = UGDR_WR_RDMA_WRITE;
    ugdr_send_wr second{};
    second.wr_id = 304;
    second.next = &third;
    second.opcode = UGDR_WR_RDMA_WRITE;
    ugdr_send_wr wrap_first{};
    wrap_first.wr_id = 303;
    wrap_first.next = &second;
    wrap_first.opcode = UGDR_WR_RDMA_WRITE;
    if (ugdr::api::post_send_chain(pair.producer, 1, &wrap_first, &bad) != 0) {
        return false;
    }
    ugdr_send_wr overflow{};
    overflow.wr_id = 306;
    overflow.opcode = UGDR_WR_RDMA_WRITE;
    if (ugdr::api::post_send_chain(pair.producer, 1, &overflow, &bad) != ENOMEM ||
        bad != &overflow) {
        return false;
    }
    ugdr::queue::ConstSlotBatch wrapped;
    if (pair.consumer.consumer_peek(3, &wrapped) != 0 || wrapped.count != 3 ||
        wrapped.first.count != 2 || wrapped.second.count != 1) {
        return false;
    }
    const std::uint32_t stride = pair.consumer.descriptor().slot_stride;
    return reinterpret_cast<const ugdr::queue::SendWqeHeader *>(slot_at(wrapped, 0, stride))
                   ->wr_id == 303 &&
           reinterpret_cast<const ugdr::queue::SendWqeHeader *>(slot_at(wrapped, 1, stride))
                   ->wr_id == 304 &&
           reinterpret_cast<const ugdr::queue::SendWqeHeader *>(slot_at(wrapped, 2, stride))
                   ->wr_id == 305 &&
           pair.consumer.consumer_release(3) == 0;
}

bool immediate_validation() {
    RingPair send_pair;
    RingPair receive_pair;
    if (!make_pair(ugdr::queue::QueueKind::send, 2, 1, &send_pair) ||
        !make_pair(ugdr::queue::QueueKind::receive, 2, 1, &receive_pair)) {
        return false;
    }

    ugdr_send_wr send{};
    send.opcode = static_cast<ugdr_wr_opcode>(99);
    auto *const send_sentinel = reinterpret_cast<ugdr_send_wr *>(static_cast<std::uintptr_t>(3));
    ugdr_send_wr *bad_send = send_sentinel;
    if (ugdr::api::post_send_chain(send_pair.producer, 1, nullptr, &bad_send) != EINVAL ||
        bad_send != send_sentinel ||
        ugdr::api::post_send_chain(send_pair.producer, 1, &send, nullptr) != EINVAL ||
        ugdr::api::post_send_chain(send_pair.producer, 1, &send, &bad_send) != EINVAL ||
        bad_send != &send) {
        return false;
    }
    send.opcode = UGDR_WR_RDMA_WRITE;
    send.send_flags = 1U << 7U;
    bad_send = nullptr;
    if (ugdr::api::post_send_chain(send_pair.producer, 1, &send, &bad_send) != EINVAL ||
        bad_send != &send) {
        return false;
    }
    send.send_flags = 0;
    send.num_sge = 1;
    if (ugdr::api::post_send_chain(send_pair.producer, 1, &send, &bad_send) != EINVAL ||
        bad_send != &send) {
        return false;
    }

    ugdr_recv_wr receive{};
    receive.num_sge = -1;
    ugdr_recv_wr *bad_receive = nullptr;
    if (ugdr::api::post_receive_chain(receive_pair.producer, 1, &receive, &bad_receive) != EINVAL ||
        bad_receive != &receive) {
        return false;
    }
    receive.num_sge = 1;
    if (ugdr::api::post_receive_chain(receive_pair.producer, 1, &receive, &bad_receive) != EINVAL ||
        bad_receive != &receive) {
        return false;
    }

    ugdr::queue::ConstSlotBatch batch;
    return send_pair.consumer.consumer_peek(1, &batch) == EAGAIN &&
           receive_pair.consumer.consumer_peek(1, &batch) == EAGAIN;
}

}  // namespace

void *operator new(std::size_t size) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
    if (void *memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void *operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void *memory) noexcept {
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void *memory) noexcept {
    std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    return send_copy_and_no_allocation() && receive_copy_and_zero_sge() &&
                   prefix_failure_full_and_wrap() && immediate_validation()
               ? 0
               : 1;
}

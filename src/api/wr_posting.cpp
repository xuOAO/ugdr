#include "api/wr_posting.hpp"

#include "queue/descriptors.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace ugdr::api {
namespace {

bool valid_sge_list(int num_sge, const ugdr_sge *sg_list, std::uint32_t max_sge) noexcept {
    return num_sge >= 0 && static_cast<std::uint32_t>(num_sge) <= max_sge &&
           (num_sge == 0 || sg_list != nullptr);
}

bool valid_send_wr(const ugdr_send_wr &wr, std::uint32_t max_sge) noexcept {
    const bool valid_opcode =
        wr.opcode == UGDR_WR_RDMA_WRITE || wr.opcode == UGDR_WR_RDMA_WRITE_WITH_IMM;
    return valid_opcode && (wr.send_flags & ~static_cast<unsigned int>(UGDR_SEND_SIGNALED)) == 0 &&
           valid_sge_list(wr.num_sge, wr.sg_list, max_sge);
}

bool valid_receive_wr(const ugdr_recv_wr &wr, std::uint32_t max_sge) noexcept {
    return valid_sge_list(wr.num_sge, wr.sg_list, max_sge);
}

void encode_send(void *slot, const ugdr_send_wr &wr) noexcept {
    auto *const header = static_cast<queue::SendWqeHeader *>(slot);
    *header = {};
    header->wr_id = wr.wr_id;
    header->remote_address = wr.wr.rdma.remote_addr;
    header->rkey = wr.wr.rdma.rkey;
    header->opcode = static_cast<std::uint32_t>(wr.opcode);
    header->send_flags = wr.send_flags;
    header->immediate_data = wr.opcode == UGDR_WR_RDMA_WRITE_WITH_IMM ? wr.imm_data : 0;
    header->sge_count = static_cast<std::uint32_t>(wr.num_sge);

    auto *const destination = reinterpret_cast<queue::SharedSge *>(header + 1);
    for (int index = 0; index < wr.num_sge; ++index) {
        destination[index] = {wr.sg_list[index].addr, wr.sg_list[index].length,
                              wr.sg_list[index].lkey};
    }
}

void encode_receive(void *slot, const ugdr_recv_wr &wr) noexcept {
    auto *const header = static_cast<queue::ReceiveWqeHeader *>(slot);
    *header = {};
    header->wr_id = wr.wr_id;
    header->sge_count = static_cast<std::uint32_t>(wr.num_sge);

    auto *const destination = reinterpret_cast<queue::SharedSge *>(header + 1);
    for (int index = 0; index < wr.num_sge; ++index) {
        destination[index] = {wr.sg_list[index].addr, wr.sg_list[index].length,
                              wr.sg_list[index].lkey};
    }
}

template <typename Wr, typename Validator, typename Encoder>
int post_chain(queue::SharedRing &ring, std::uint32_t max_sge, Wr *wr, Wr **bad_wr,
               Validator validator, Encoder encoder) noexcept {
    if (wr == nullptr || bad_wr == nullptr || !ring.valid()) {
        return EINVAL;
    }

    Wr *current = wr;
    const std::uint32_t stride = ring.descriptor().slot_stride;
    while (current != nullptr) {
        queue::MutableSlotBatch batch;
        const int reserve_status = ring.producer_reserve(ring.descriptor().capacity, &batch);
        if (reserve_status != 0) {
            *bad_wr = current;
            return reserve_status == EAGAIN ? ENOMEM : reserve_status;
        }

        std::uint32_t accepted = 0;
        auto fill_span = [&](queue::MutableSlotSpan span) noexcept -> bool {
            auto *slot = static_cast<std::byte *>(span.data);
            for (std::uint32_t index = 0; index < span.count && current != nullptr; ++index) {
                if (!validator(*current, max_sge)) {
                    return false;
                }
                encoder(slot + static_cast<std::size_t>(index) * stride, *current);
                ++accepted;
                current = current->next;
            }
            return true;
        };

        const bool first_valid = fill_span(batch.first);
        const bool second_valid = first_valid && fill_span(batch.second);
        const int publish_status = ring.producer_publish(accepted);
        if (publish_status != 0) {
            *bad_wr = current;
            return publish_status;
        }
        if (!first_valid || !second_valid) {
            *bad_wr = current;
            return EINVAL;
        }
    }
    return 0;
}

}  // namespace

int post_send_chain(queue::SharedRing &ring, std::uint32_t max_sge, ugdr_send_wr *wr,
                    ugdr_send_wr **bad_wr) noexcept {
    if (ring.descriptor().kind != queue::QueueKind::send) {
        return EINVAL;
    }
    return post_chain(ring, max_sge, wr, bad_wr, valid_send_wr, encode_send);
}

int post_receive_chain(queue::SharedRing &ring, std::uint32_t max_sge, ugdr_recv_wr *wr,
                       ugdr_recv_wr **bad_wr) noexcept {
    if (ring.descriptor().kind != queue::QueueKind::receive) {
        return EINVAL;
    }
    return post_chain(ring, max_sge, wr, bad_wr, valid_receive_wr, encode_receive);
}

}  // namespace ugdr::api

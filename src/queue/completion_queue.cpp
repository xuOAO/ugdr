#include "queue/completion_queue.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>

namespace ugdr::queue {
namespace {

void copy_entries(const MutableSlotSpan &span, const CompletionEntry *entries, std::size_t *offset,
                  std::uint32_t slot_stride) noexcept {
    auto *slots = static_cast<std::byte *>(span.data);
    for (std::uint32_t index = 0; index < span.count; ++index) {
        std::memcpy(slots + static_cast<std::size_t>(index) * slot_stride, entries + *offset,
                    sizeof(CompletionEntry));
        ++*offset;
    }
}

}  // namespace

int produce_completions(SharedRing &ring, const CompletionEntry *entries,
                        int num_entries) noexcept {
    if (num_entries < 0 || (num_entries > 0 && entries == nullptr)) {
        return -EINVAL;
    }
    if (num_entries == 0) {
        return 0;
    }
    const QueueDescriptor &descriptor = ring.descriptor();
    if (!ring.valid() || descriptor.kind != QueueKind::completion ||
        descriptor.slot_stride != completion_slot_stride()) {
        return -EINVAL;
    }

    MutableSlotBatch batch;
    const int reserve_status =
        ring.producer_reserve(static_cast<std::uint32_t>(num_entries), &batch);
    if (reserve_status == EAGAIN) {
        return 0;
    }
    if (reserve_status != 0) {
        return -reserve_status;
    }

    std::size_t offset = 0;
    copy_entries(batch.first, entries, &offset, descriptor.slot_stride);
    copy_entries(batch.second, entries, &offset, descriptor.slot_stride);
    const int publish_status = ring.producer_publish(batch.count);
    return publish_status == 0 ? static_cast<int>(batch.count) : -publish_status;
}

}  // namespace ugdr::queue

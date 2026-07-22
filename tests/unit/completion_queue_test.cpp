#include "queue/completion_queue.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>

namespace {

using ugdr::queue::CompletionEntry;
using ugdr::queue::ConstSlotBatch;
using ugdr::queue::ConstSlotSpan;
using ugdr::queue::QueueDescriptor;
using ugdr::queue::QueueKind;
using ugdr::queue::SharedRing;

CompletionEntry entry(std::uint64_t id) {
    return {id,
            1,
            129,
            static_cast<std::uint32_t>(id + 10),
            static_cast<std::uint32_t>(id + 20),
            static_cast<std::uint32_t>(id + 30),
            2};
}

bool verify_span(ConstSlotSpan span, const CompletionEntry *expected, std::size_t *offset,
                 std::uint32_t stride) {
    const auto *slots = static_cast<const std::byte *>(span.data);
    for (std::uint32_t index = 0; index < span.count; ++index) {
        CompletionEntry actual;
        std::memcpy(&actual, slots + static_cast<std::size_t>(index) * stride, sizeof(actual));
        if (std::memcmp(&actual, expected + *offset, sizeof(actual)) != 0) {
            return false;
        }
        ++*offset;
    }
    return true;
}

bool consume(SharedRing &ring, const CompletionEntry *expected, std::uint32_t count) {
    ConstSlotBatch batch;
    if (ring.consumer_peek(count, &batch) != 0 || batch.count != count) {
        return false;
    }
    std::size_t offset = 0;
    return verify_span(batch.first, expected, &offset, ring.descriptor().slot_stride) &&
           verify_span(batch.second, expected, &offset, ring.descriptor().slot_stride) &&
           offset == count && ring.consumer_release(count) == 0;
}

int batch_and_capacity_test() {
    const QueueDescriptor descriptor{QueueKind::completion, 5,
                                     ugdr::queue::completion_slot_stride()};
    SharedRing ring;
    if (ugdr::queue::create_shared_ring(descriptor, &ring) != 0) {
        return 1;
    }
    const std::array first{entry(1), entry(2), entry(3), entry(4)};
    if (ugdr::queue::produce_completions(ring, first.data(), first.size()) != 4 ||
        !consume(ring, first.data(), 2)) {
        return 2;
    }
    const std::array second{entry(5), entry(6), entry(7), entry(8)};
    if (ugdr::queue::produce_completions(ring, second.data(), second.size()) != 3 ||
        ugdr::queue::produce_completions(ring, second.data() + 3, 1) != 0) {
        return 3;
    }
    const std::array expected{entry(3), entry(4), entry(5), entry(6), entry(7)};
    if (!consume(ring, expected.data(), expected.size())) {
        return 4;
    }
    return ugdr::queue::produce_completions(ring, second.data() + 3, 1) == 1 &&
                   consume(ring, second.data() + 3, 1)
               ? 0
               : 5;
}

int validation_test() {
    SharedRing invalid;
    if (ugdr::queue::produce_completions(invalid, nullptr, -1) != -EINVAL ||
        ugdr::queue::produce_completions(invalid, nullptr, 1) != -EINVAL ||
        ugdr::queue::produce_completions(invalid, nullptr, 0) != 0) {
        return 1;
    }
    const QueueDescriptor descriptor{QueueKind::send, 1, ugdr::queue::completion_slot_stride()};
    SharedRing wrong_kind;
    const CompletionEntry value = entry(11);
    return ugdr::queue::create_shared_ring(descriptor, &wrong_kind) == 0 &&
                   ugdr::queue::produce_completions(wrong_kind, &value, 1) == -EINVAL
               ? 0
               : 2;
}

}  // namespace

int main() {
    if (batch_and_capacity_test() != 0) {
        return 1;
    }
    return validation_test() == 0 ? 0 : 2;
}

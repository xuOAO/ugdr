#include "queue/shared_ring.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstring>
#include <limits>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace ugdr::queue {
namespace {

static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);

bool valid_descriptor(const QueueDescriptor &descriptor) noexcept {
    const auto kind = static_cast<std::uint16_t>(descriptor.kind);
    return kind >= static_cast<std::uint16_t>(QueueKind::send) &&
           kind <= static_cast<std::uint16_t>(QueueKind::completion) && descriptor.capacity != 0 &&
           descriptor.slot_stride != 0 && descriptor.slot_stride % kSharedRingCacheLine == 0;
}

int system_page_size(std::size_t *page_size) noexcept {
    const long value = sysconf(_SC_PAGESIZE);
    if (value <= 0) {
        return errno == 0 ? EINVAL : errno;
    }
    *page_size = static_cast<std::size_t>(value);
    return 0;
}

int create_memfd() noexcept {
#ifdef SYS_memfd_create
    return static_cast<int>(
        syscall(SYS_memfd_create, "ugdr-ring", MFD_CLOEXEC | MFD_ALLOW_SEALING));
#else
    errno = ENOSYS;
    return -1;
#endif
}

bool reserved_is_zero(const SharedRingHeader &header) noexcept {
    if (header.metadata.reserved0 != 0) {
        return false;
    }
    for (std::uint64_t value : header.metadata.reserved) {
        if (value != 0) {
            return false;
        }
    }
    for (std::uint64_t value : header.tail.reserved) {
        if (value != 0) {
            return false;
        }
    }
    for (std::uint64_t value : header.head.reserved) {
        if (value != 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

SharedRing::SharedRing(void *mapping, std::size_t mapping_size, int descriptor,
                       QueueDescriptor queue_descriptor) noexcept
    : mapping_(mapping), mapping_size_(mapping_size), descriptor_(descriptor),
      queue_descriptor_(queue_descriptor) {
}

SharedRing::~SharedRing() {
    reset();
}

SharedRing::SharedRing(SharedRing &&other) noexcept {
    *this = std::move(other);
}

SharedRing &SharedRing::operator=(SharedRing &&other) noexcept {
    if (this != &other) {
        reset();
        mapping_ = std::exchange(other.mapping_, nullptr);
        mapping_size_ = std::exchange(other.mapping_size_, 0);
        descriptor_ = std::exchange(other.descriptor_, -1);
        queue_descriptor_ = other.queue_descriptor_;
        producer_ = other.producer_;
        consumer_ = other.consumer_;
        other.producer_ = {};
        other.consumer_ = {};
    }
    return *this;
}

void SharedRing::reset() noexcept {
    if (mapping_ != nullptr) {
        (void)munmap(mapping_, mapping_size_);
    }
    if (descriptor_ >= 0) {
        (void)::close(descriptor_);
    }
    mapping_ = nullptr;
    mapping_size_ = 0;
    descriptor_ = -1;
    queue_descriptor_ = {};
    producer_ = {};
    consumer_ = {};
}

bool SharedRing::valid() const noexcept {
    return mapping_ != nullptr;
}

const QueueDescriptor &SharedRing::descriptor() const noexcept {
    return queue_descriptor_;
}

std::size_t SharedRing::mapping_size() const noexcept {
    return mapping_size_;
}

const void *SharedRing::mapping_address() const noexcept {
    return mapping_;
}

int SharedRing::duplicate_fd(int *descriptor) const noexcept {
    if (descriptor == nullptr || descriptor_ < 0) {
        return EINVAL;
    }
    const int copy = fcntl(descriptor_, F_DUPFD_CLOEXEC, 0);
    if (copy < 0) {
        return errno;
    }
    *descriptor = copy;
    return 0;
}

SharedRingHeader *SharedRing::header() noexcept {
    return static_cast<SharedRingHeader *>(mapping_);
}

const SharedRingHeader *SharedRing::header() const noexcept {
    return static_cast<const SharedRingHeader *>(mapping_);
}

void *SharedRing::slot_at(std::uint32_t index) noexcept {
    auto *bytes = static_cast<std::byte *>(mapping_);
    return bytes + sizeof(SharedRingHeader) +
           static_cast<std::size_t>(index) * queue_descriptor_.slot_stride;
}

int SharedRing::producer_reserve(std::uint32_t max_count, MutableSlotBatch *batch) noexcept {
    if (!valid() || max_count == 0 || batch == nullptr || producer_.reserved != 0) {
        return EINVAL;
    }
    std::atomic_ref<std::uint64_t> shared_tail(header()->tail.value);
    std::atomic_ref<std::uint64_t> shared_head(header()->head.value);
    if (!producer_.initialized) {
        producer_.local_tail = shared_tail.load(std::memory_order_relaxed);
        producer_.cached_head = shared_head.load(std::memory_order_acquire);
        producer_.local_index =
            static_cast<std::uint32_t>(producer_.local_tail % queue_descriptor_.capacity);
        producer_.initialized = true;
    }
    std::uint64_t used = producer_.local_tail - producer_.cached_head;
    if (used > queue_descriptor_.capacity) {
        return EPROTO;
    }
    std::uint64_t available = queue_descriptor_.capacity - used;
    if (available < max_count) {
        producer_.cached_head = shared_head.load(std::memory_order_acquire);
        used = producer_.local_tail - producer_.cached_head;
        if (used > queue_descriptor_.capacity) {
            return EPROTO;
        }
        available = queue_descriptor_.capacity - used;
    }
    if (available == 0) {
        return EAGAIN;
    }
    const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(max_count, available));
    const auto start = producer_.local_index;
    const auto first_count = std::min(count, queue_descriptor_.capacity - start);
    MutableSlotBatch reserved;
    reserved.first = {slot_at(start), first_count};
    reserved.second = {first_count == count ? nullptr : slot_at(0), count - first_count};
    reserved.count = count;
    producer_.reserved = count;
    *batch = reserved;
    return 0;
}

int SharedRing::producer_publish(std::uint32_t count) noexcept {
    if (!valid() || producer_.reserved == 0 || count > producer_.reserved) {
        return EINVAL;
    }
    if (count != 0) {
        producer_.local_tail += count;
        producer_.local_index += count;
        if (producer_.local_index >= queue_descriptor_.capacity) {
            producer_.local_index -= queue_descriptor_.capacity;
        }
        std::atomic_ref<std::uint64_t> shared_tail(header()->tail.value);
        shared_tail.store(producer_.local_tail, std::memory_order_release);
    }
    producer_.reserved = 0;
    return 0;
}

int SharedRing::consumer_peek(std::uint32_t max_count, ConstSlotBatch *batch) noexcept {
    if (!valid() || max_count == 0 || batch == nullptr || consumer_.peeked != 0) {
        return EINVAL;
    }
    std::atomic_ref<std::uint64_t> shared_tail(header()->tail.value);
    std::atomic_ref<std::uint64_t> shared_head(header()->head.value);
    if (!consumer_.initialized) {
        consumer_.local_head = shared_head.load(std::memory_order_relaxed);
        consumer_.cached_tail = shared_tail.load(std::memory_order_acquire);
        consumer_.local_index =
            static_cast<std::uint32_t>(consumer_.local_head % queue_descriptor_.capacity);
        consumer_.initialized = true;
    }
    std::uint64_t available = consumer_.cached_tail - consumer_.local_head;
    if (available > queue_descriptor_.capacity) {
        return EPROTO;
    }
    if (available < max_count) {
        consumer_.cached_tail = shared_tail.load(std::memory_order_acquire);
        available = consumer_.cached_tail - consumer_.local_head;
        if (available > queue_descriptor_.capacity) {
            return EPROTO;
        }
    }
    if (available == 0) {
        return EAGAIN;
    }
    const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(max_count, available));
    const auto start = consumer_.local_index;
    const auto first_count = std::min(count, queue_descriptor_.capacity - start);
    ConstSlotBatch visible;
    visible.first = {slot_at(start), first_count};
    visible.second = {first_count == count ? nullptr : slot_at(0), count - first_count};
    visible.count = count;
    consumer_.peeked = count;
    *batch = visible;
    return 0;
}

int SharedRing::consumer_release(std::uint32_t count) noexcept {
    if (!valid() || consumer_.peeked == 0 || count > consumer_.peeked) {
        return EINVAL;
    }
    if (count != 0) {
        consumer_.local_head += count;
        consumer_.local_index += count;
        if (consumer_.local_index >= queue_descriptor_.capacity) {
            consumer_.local_index -= queue_descriptor_.capacity;
        }
        std::atomic_ref<std::uint64_t> shared_head(header()->head.value);
        shared_head.store(consumer_.local_head, std::memory_order_release);
    }
    consumer_.peeked = 0;
    return 0;
}

int SharedRing::producer_reserve(void **slot_pointer) noexcept {
    if (slot_pointer == nullptr) {
        return EINVAL;
    }
    MutableSlotBatch batch;
    const int status = producer_reserve(1, &batch);
    if (status == 0) {
        *slot_pointer = batch.first.data;
    }
    return status;
}

int SharedRing::producer_publish() noexcept {
    return producer_publish(1);
}

int SharedRing::consumer_peek(const void **slot_pointer) noexcept {
    if (slot_pointer == nullptr) {
        return EINVAL;
    }
    ConstSlotBatch batch;
    const int status = consumer_peek(1, &batch);
    if (status == 0) {
        *slot_pointer = batch.first.data;
    }
    return status;
}

int SharedRing::consumer_release() noexcept {
    return consumer_release(1);
}

int shared_ring_mapping_size(const QueueDescriptor &descriptor, std::size_t page_size,
                             std::size_t *mapping_size) noexcept {
    if (mapping_size == nullptr || page_size == 0 || !valid_descriptor(descriptor)) {
        return EINVAL;
    }
    if (descriptor.capacity > (std::numeric_limits<std::size_t>::max() - sizeof(SharedRingHeader)) /
                                  descriptor.slot_stride) {
        return EOVERFLOW;
    }
    const std::size_t raw = sizeof(SharedRingHeader) +
                            static_cast<std::size_t>(descriptor.capacity) * descriptor.slot_stride;
    const std::size_t remainder = raw % page_size;
    if (remainder != 0 && raw > std::numeric_limits<std::size_t>::max() - (page_size - remainder)) {
        return EOVERFLOW;
    }
    *mapping_size = remainder == 0 ? raw : raw + page_size - remainder;
    return 0;
}

int create_shared_ring(const QueueDescriptor &descriptor, SharedRing *ring) noexcept {
    if (ring == nullptr || ring->valid()) {
        return EINVAL;
    }
    std::size_t page_size = 0;
    int status = system_page_size(&page_size);
    std::size_t mapping_size = 0;
    if (status == 0) {
        status = shared_ring_mapping_size(descriptor, page_size, &mapping_size);
    }
    if (status != 0) {
        return status;
    }
    if (mapping_size > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
        return EOVERFLOW;
    }
    const int descriptor_fd = create_memfd();
    if (descriptor_fd < 0) {
        return errno;
    }
    if (ftruncate(descriptor_fd, static_cast<off_t>(mapping_size)) != 0) {
        status = errno;
        (void)::close(descriptor_fd);
        return status;
    }
    void *mapping =
        mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor_fd, 0);
    if (mapping == MAP_FAILED) {
        status = errno;
        (void)::close(descriptor_fd);
        return status;
    }
    std::memset(mapping, 0, mapping_size);
    auto *header = static_cast<SharedRingHeader *>(mapping);
    header->metadata.magic = kSharedRingMagic;
    header->metadata.version = kSharedRingVersion;
    header->metadata.kind = static_cast<std::uint16_t>(descriptor.kind);
    header->metadata.header_bytes = sizeof(SharedRingHeader);
    header->metadata.mapping_bytes = mapping_size;
    header->metadata.capacity = descriptor.capacity;
    header->metadata.slot_stride = descriptor.slot_stride;
    if (fcntl(descriptor_fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
        status = errno;
        (void)munmap(mapping, mapping_size);
        (void)::close(descriptor_fd);
        return status;
    }
    *ring = SharedRing(mapping, mapping_size, descriptor_fd, descriptor);
    return 0;
}

int map_shared_ring(int descriptor_fd, const QueueDescriptor &expected, SharedRing *ring) noexcept {
    if (descriptor_fd < 0 || ring == nullptr || ring->valid() || !valid_descriptor(expected)) {
        return EINVAL;
    }
    struct stat status_buffer {};
    if (fstat(descriptor_fd, &status_buffer) != 0) {
        return errno;
    }
    if (status_buffer.st_size < static_cast<off_t>(sizeof(SharedRingHeader)) ||
        static_cast<std::uintmax_t>(status_buffer.st_size) >
            std::numeric_limits<std::size_t>::max()) {
        return EPROTO;
    }
    const std::size_t mapping_size = static_cast<std::size_t>(status_buffer.st_size);
    void *mapping =
        mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor_fd, 0);
    if (mapping == MAP_FAILED) {
        return errno;
    }
    const auto *header = static_cast<const SharedRingHeader *>(mapping);
    int validation = 0;
    if (header->metadata.magic == kSharedRingMagic &&
        header->metadata.version != kSharedRingVersion) {
        validation = EPROTONOSUPPORT;
    } else if (header->metadata.magic != kSharedRingMagic ||
               header->metadata.kind != static_cast<std::uint16_t>(expected.kind) ||
               header->metadata.header_bytes != sizeof(SharedRingHeader) ||
               header->metadata.mapping_bytes != mapping_size ||
               header->metadata.capacity != expected.capacity ||
               header->metadata.slot_stride != expected.slot_stride || !reserved_is_zero(*header)) {
        validation = EPROTO;
    }
    std::size_t page_size = 0;
    std::size_t expected_size = 0;
    if (validation == 0) {
        validation = system_page_size(&page_size);
    }
    if (validation == 0) {
        validation = shared_ring_mapping_size(expected, page_size, &expected_size);
    }
    if (validation == 0 && expected_size != mapping_size) {
        validation = EPROTO;
    }
    if (validation != 0) {
        (void)munmap(mapping, mapping_size);
        return validation;
    }
    *ring = SharedRing(mapping, mapping_size, -1, expected);
    return 0;
}

}  // namespace ugdr::queue

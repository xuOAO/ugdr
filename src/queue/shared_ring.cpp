#include "queue/shared_ring.hpp"

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
    for (std::uint64_t value : header.producer.reserved) {
        if (value != 0) {
            return false;
        }
    }
    for (std::uint64_t value : header.consumer.reserved) {
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
        producer_reserved_ = std::exchange(other.producer_reserved_, false);
        consumer_peeked_ = std::exchange(other.consumer_peeked_, false);
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
    producer_reserved_ = false;
    consumer_peeked_ = false;
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

void *SharedRing::slot(std::uint64_t position) noexcept {
    auto *bytes = static_cast<std::byte *>(mapping_);
    return bytes + sizeof(SharedRingHeader) +
           (position % queue_descriptor_.capacity) * queue_descriptor_.slot_stride;
}

int SharedRing::producer_reserve(void **slot_pointer) noexcept {
    if (!valid() || slot_pointer == nullptr || producer_reserved_) {
        return EINVAL;
    }
    std::atomic_ref<std::uint64_t> producer(header()->producer.value);
    std::atomic_ref<std::uint64_t> consumer(header()->consumer.value);
    const std::uint64_t produced = producer.load(std::memory_order_relaxed);
    const std::uint64_t consumed = consumer.load(std::memory_order_acquire);
    if (produced - consumed >= queue_descriptor_.capacity) {
        return EAGAIN;
    }
    *slot_pointer = slot(produced);
    producer_reserved_ = true;
    return 0;
}

int SharedRing::producer_publish() noexcept {
    if (!valid() || !producer_reserved_) {
        return EINVAL;
    }
    std::atomic_ref<std::uint64_t> producer(header()->producer.value);
    producer.fetch_add(1, std::memory_order_release);
    producer_reserved_ = false;
    return 0;
}

int SharedRing::consumer_peek(const void **slot_pointer) noexcept {
    if (!valid() || slot_pointer == nullptr || consumer_peeked_) {
        return EINVAL;
    }
    std::atomic_ref<std::uint64_t> producer(header()->producer.value);
    std::atomic_ref<std::uint64_t> consumer(header()->consumer.value);
    const std::uint64_t consumed = consumer.load(std::memory_order_relaxed);
    const std::uint64_t produced = producer.load(std::memory_order_acquire);
    if (produced == consumed) {
        return EAGAIN;
    }
    *slot_pointer = slot(consumed);
    consumer_peeked_ = true;
    return 0;
}

int SharedRing::consumer_release() noexcept {
    if (!valid() || !consumer_peeked_) {
        return EINVAL;
    }
    std::atomic_ref<std::uint64_t> consumer(header()->consumer.value);
    consumer.fetch_add(1, std::memory_order_release);
    consumer_peeked_ = false;
    return 0;
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

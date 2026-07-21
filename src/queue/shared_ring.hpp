#pragma once

#include <cstddef>
#include <cstdint>

namespace ugdr::queue {

constexpr std::uint32_t kSharedRingMagic = UINT32_C(0x55475251);
constexpr std::uint16_t kSharedRingVersion = 1;
constexpr std::size_t kSharedRingCacheLine = 64;

enum class QueueKind : std::uint16_t {
    send = 1,
    receive = 2,
    completion = 3,
};

struct QueueDescriptor {
    QueueKind kind = QueueKind::send;
    std::uint32_t capacity = 0;
    std::uint32_t slot_stride = 0;

    bool operator==(const QueueDescriptor &) const = default;
};

struct alignas(kSharedRingCacheLine) SharedRingMetadata {
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t kind = 0;
    std::uint32_t header_bytes = 0;
    std::uint32_t reserved0 = 0;
    std::uint64_t mapping_bytes = 0;
    std::uint32_t capacity = 0;
    std::uint32_t slot_stride = 0;
    std::uint64_t reserved[4]{};
};

struct alignas(kSharedRingCacheLine) SharedRingPosition {
    std::uint64_t value = 0;
    std::uint64_t reserved[7]{};
};

struct alignas(kSharedRingCacheLine) SharedRingHeader {
    SharedRingMetadata metadata;
    SharedRingPosition producer;
    SharedRingPosition consumer;
};

static_assert(sizeof(SharedRingMetadata) == kSharedRingCacheLine);
static_assert(sizeof(SharedRingPosition) == kSharedRingCacheLine);
static_assert(sizeof(SharedRingHeader) == 3 * kSharedRingCacheLine);

class SharedRing {
  public:
    SharedRing() noexcept = default;
    ~SharedRing();

    SharedRing(const SharedRing &) = delete;
    SharedRing &operator=(const SharedRing &) = delete;
    SharedRing(SharedRing &&other) noexcept;
    SharedRing &operator=(SharedRing &&other) noexcept;

    void reset() noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const QueueDescriptor &descriptor() const noexcept;
    [[nodiscard]] std::size_t mapping_size() const noexcept;
    [[nodiscard]] const void *mapping_address() const noexcept;
    int duplicate_fd(int *descriptor) const noexcept;

    int producer_reserve(void **slot) noexcept;
    int producer_publish() noexcept;
    int consumer_peek(const void **slot) noexcept;
    int consumer_release() noexcept;

  private:
    friend int create_shared_ring(const QueueDescriptor &, SharedRing *) noexcept;
    friend int map_shared_ring(int, const QueueDescriptor &, SharedRing *) noexcept;

    SharedRing(void *mapping, std::size_t mapping_size, int descriptor,
               QueueDescriptor queue_descriptor) noexcept;
    [[nodiscard]] SharedRingHeader *header() noexcept;
    [[nodiscard]] const SharedRingHeader *header() const noexcept;
    [[nodiscard]] void *slot(std::uint64_t position) noexcept;

    void *mapping_ = nullptr;
    std::size_t mapping_size_ = 0;
    int descriptor_ = -1;
    QueueDescriptor queue_descriptor_{};
    bool producer_reserved_ = false;
    bool consumer_peeked_ = false;
};

int shared_ring_mapping_size(const QueueDescriptor &descriptor, std::size_t page_size,
                             std::size_t *mapping_size) noexcept;
int create_shared_ring(const QueueDescriptor &descriptor, SharedRing *ring) noexcept;
int map_shared_ring(int descriptor, const QueueDescriptor &expected, SharedRing *ring) noexcept;

}  // namespace ugdr::queue

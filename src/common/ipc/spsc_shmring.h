#pragma once
#include <atomic>
#include <algorithm>
#include "shm.h"

namespace ugdr{
namespace ipc{

template <typename T>
class SpscShmRing : public Shmem {
public:
    SpscShmRing(std::string& name, uint32_t nb_elem)
        : Shmem(name, sizeof(RingLayout) + (nb_elem + 1) * sizeof(T)) {
        layout_ = new(addr_) RingLayout();
        layout_->head = 0;
        layout_->tail = 0;
        layout_->capacity = nb_elem + 1;

        buffer_ = reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(addr_) + sizeof(RingLayout));

        capacity_ = layout_->capacity;
    }
    SpscShmRing(std::string& name, uint32_t nb_elem, int fd)
        : Shmem(name, sizeof(RingLayout) + (nb_elem + 1) * sizeof(T), fd) {
        layout_ = reinterpret_cast<RingLayout*>(addr_);
        buffer_ = reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(addr_) + sizeof(RingLayout));

        capacity_ = layout_->capacity;
        // sync_indices();
    }
    
    SpscShmRing(SpscShmRing&& other) noexcept : Shmem(std::move(other)) {
        layout_ = other.layout_;
        buffer_ = other.buffer_;
        capacity_ = other.capacity_;
        local_head = other.local_head;
        local_tail = other.local_tail;
        shadow_head = other.shadow_head;
        shadow_tail = other.shadow_tail;

        other.layout_ = nullptr;
        other.buffer_ = nullptr;
        other.capacity_ = 0;
        other.local_head = 0;
        other.local_tail = 0;
        other.shadow_head = 0;
        other.shadow_tail = 0;
    }

    SpscShmRing& operator=(SpscShmRing&& other) noexcept {
        if (this != &other) {
            Shmem::operator=(std::move(other));
            layout_ = other.layout_;
            buffer_ = other.buffer_;
            capacity_ = other.capacity_;
            local_head = other.local_head;
            local_tail = other.local_tail;
            shadow_head = other.shadow_head;
            shadow_tail = other.shadow_tail;

            other.layout_ = nullptr;
            other.buffer_ = nullptr;
            other.capacity_ = 0;
            other.local_head = 0;
            other.local_tail = 0;
            other.shadow_head = 0;
            other.shadow_tail = 0;
        }
        return *this;
    }

    virtual ~SpscShmRing() {
        // layout_ is in shared memory, managed by Shmem (mmap/munmap)
        // do not delete layout_;
    }
    bool push(const T* item) {
        bool success = true;
        const uint32_t next_tail = (local_tail + 1) % (capacity_);
        if (next_tail == shadow_head) [[unlikely]] {
            // update shadow_head
            shadow_head = layout_->head.load(std::memory_order_acquire);
            if (next_tail == shadow_head) [[unlikely]] {
                success = false;
                return success; // ring is full
            }
        }

        buffer_[local_tail] = *item;
        layout_->tail.store(next_tail, std::memory_order_release);
        local_tail = next_tail;

        return success;
    }
    bool pop(T* item) {
        bool success = true;
        if (local_head == shadow_tail) [[unlikely]] {
            // update shadow_tail
            shadow_tail = layout_->tail.load(std::memory_order_acquire);
            if (local_head == shadow_tail) [[unlikely]] {
                success = false;
                return success; // ring is empty
            }
        }

        *item = buffer_[local_head];
        const uint32_t next_head = (local_head + 1) % (capacity_);
        layout_->head.store(next_head, std::memory_order_release);
        local_head = next_head;

        return success;
    }
    int push_batch(const T* items, uint32_t n) {
        if (n == 0) return 0;

        uint32_t available;
        const uint32_t cap = capacity_;

        if (shadow_head <= local_tail) {
            available = (cap - 1) - (local_tail - shadow_head);
        } else {
            available = shadow_head - local_tail - 1;
        }

        if (available < n) [[unlikely]] {
            // update shadow_head
            shadow_head = layout_->head.load(std::memory_order_acquire);
            if (shadow_head <= local_tail) {
                available = (cap - 1) - (local_tail - shadow_head);
            } else {
                available = shadow_head - local_tail - 1;
            }
        }

        const uint32_t count = std::min(available, n);
        if (count == 0) return 0;

        const uint32_t to_end = cap - local_tail;
        const uint32_t first_chunk = std::min(count, to_end);

        std::copy_n(items, first_chunk, &buffer_[local_tail]);

        if (count > first_chunk) {
            std::copy_n(items + first_chunk, count - first_chunk, &buffer_[0]);
        }

        local_tail = (local_tail + count) % cap;
        layout_->tail.store(local_tail,  std::memory_order_release);

        return count;
    }
    int pop_batch(T* items, uint32_t n) {
        if (n == 0) return 0;

        uint32_t available;
        const uint32_t cap = capacity_;

        if (shadow_tail >= local_head) {
            available = shadow_tail - local_head;
        } else {
            available = shadow_tail + (cap - local_head);
        }

        if (available < n) [[unlikely]] {
            // update shadow_tail
            shadow_tail = layout_->tail.load(std::memory_order_acquire);
            if (shadow_tail >= local_head) {
                available = shadow_tail - local_head;
            } else {
                available = shadow_tail + (cap - local_head);
            }
        }

        const uint32_t count = std::min(available, n);
        if (count == 0) return 0;

        const uint32_t to_end = cap - local_head;
        const uint32_t first_chunk = std::min(count, to_end);

        std::copy_n(&buffer_[local_head], first_chunk, items);

        if (count > first_chunk) {
            std::copy_n(&buffer_[0], count - first_chunk, items + first_chunk);
        }

        local_head = (local_head + count) % cap;
        layout_->head.store(local_head,  std::memory_order_release);

        return count;
    }

private:
    void sync_indices() {
        if (layout_) {
            local_head = layout_->head.load(std::memory_order_acquire);
            local_tail = layout_->tail.load(std::memory_order_acquire);
            shadow_head = local_head;
            shadow_tail = local_tail;
        }
    }
    struct alignas(CACHELINE_SIZE) RingLayout {
        //解决伪共享
        alignas(CACHELINE_SIZE) std::atomic<uint32_t> head;
        alignas(CACHELINE_SIZE) std::atomic<uint32_t> tail;
        alignas(CACHELINE_SIZE) uint32_t capacity;
    };

    RingLayout* layout_ = nullptr;
    T* buffer_ = nullptr;

    uint32_t local_head = 0;
    uint32_t shadow_head = 0;

    uint32_t local_tail = 0;
    uint32_t shadow_tail = 0;

    uint32_t capacity_;
};

} // namespace ipc
} // namespace ugdr
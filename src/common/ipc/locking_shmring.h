#pragma once
#include <atomic>
#include "shm.h"

namespace ugdr{
namespace ipc{

template <typename T>
class LockingShmRing : public Shmem {
public:
    LockingShmRing(std::string& name, uint32_t nb_elem)
        : Shmem(name, sizeof(RingLayout) + (nb_elem + 1) * sizeof(T)) {
        layout_ = new(addr_) RingLayout();
        layout_->lock.clear();
        layout_->head = 0;
        layout_->tail = 0;
        layout_->capacity = nb_elem + 1;

        buffer_ = reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(addr_) + sizeof(RingLayout));
    }
    LockingShmRing(std::string& name, uint32_t nb_elem, int fd)
        : Shmem(name, sizeof(RingLayout) + (nb_elem + 1) * sizeof(T), fd) {
        layout_ = reinterpret_cast<RingLayout*>(addr_);
        buffer_ = reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(addr_) + sizeof(RingLayout));
    }
    
    LockingShmRing(LockingShmRing&& other) noexcept : Shmem(std::move(other)) {
        layout_ = other.layout_;
        buffer_ = other.buffer_;
        other.layout_ = nullptr;
        other.buffer_ = nullptr;
    }

    LockingShmRing& operator=(LockingShmRing&& other) noexcept {
        if (this != &other) {
            Shmem::operator=(std::move(other));
            layout_ = other.layout_;
            buffer_ = other.buffer_;
            other.layout_ = nullptr;
            other.buffer_ = nullptr;
        }
        return *this;
    }

    virtual ~LockingShmRing() {
        // layout_ is in shared memory, managed by Shmem (mmap/munmap)
        // do not delete layout_;
    }
    bool push(const T* item) {
        while(layout_->lock.test_and_set(std::memory_order_acquire));

        bool success = true;
        uint32_t next_tail = (layout_->tail + 1) % (layout_->capacity);
        if (next_tail == layout_->head) {
            success = false;
            layout_->lock.clear(std::memory_order_release);
            return success; // ring is full
        }

        buffer_[layout_->tail] = *item;
        layout_->tail = next_tail;

        layout_->lock.clear(std::memory_order_release);
        return success;
    }
    bool pop(T* item) {
        while(layout_->lock.test_and_set(std::memory_order_acquire));

        bool success = true;
        if (layout_->head == layout_->tail) {
            success = false;
            layout_->lock.clear(std::memory_order_release);
            return success; // ring is empty
        }

        *item = buffer_[layout_->head];
        layout_->head = (layout_->head + 1) % (layout_->capacity);

        layout_->lock.clear(std::memory_order_release);
        return success;
    }
private:
    struct alignas(CACHELINE_SIZE) RingLayout {
        std::atomic_flag lock;

        uint32_t head;
        uint32_t tail;
        uint32_t capacity;
    };

    RingLayout* layout_ = nullptr;
    T* buffer_ = nullptr;
};

} // namespace ipc
} // namespace ugdr
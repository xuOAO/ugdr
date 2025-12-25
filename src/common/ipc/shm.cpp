#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include "shm.h"


namespace ugdr{
namespace ipc{

static inline size_t align_to_page(size_t size) {
    constexpr size_t PAGE_SIZE = 4096;
    return (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

void Shmem::map_memory(){
    // 1. mmap
    addr_ = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (addr_ == MAP_FAILED) {
        throw_error(name_ + " failed to mmap: " + strerror(errno));
    }
}

void Shmem::cleanup(){
    // 1. munmap
    int ret = 0;
    if (addr_) {
        ret = ::munmap(addr_, size_);
        if (ret < 0) {
            throw_error(name_ + "failed to munmap");
        }
        addr_ = nullptr;
    }
    // 2. close fd_
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void Shmem::move_from(Shmem&& other) noexcept{
    // 1. set members from other to this
    fd_ = other.fd_;
    size_ = other.size_;
    addr_ = other.addr_;
    name_ = std::move(other.name_);
    // 2. set other's members to default values
    other.fd_ = -1;
    other.size_ = 0;
    other.addr_ = nullptr;
}

Shmem::Shmem(std::string name, size_t size) : name_(name) {
    // 1. create mem_fd
    size_ = align_to_page(size);
    fd_ = memfd_create(name.c_str(), MFD_CLOEXEC);
    if (fd_ < 0) {
        throw_error(name_ + " failed to create memfd");
    }
    // 2. set mem_fd size
    if (ftruncate(fd_, size_) < 0) {
        throw_error(name_ + "failed to ftruncate: size = " + std::to_string(size_));
    }
    // 3. map memory
    map_memory();
}

Shmem::Shmem(std::string name, size_t size, int fd) : name_(name), fd_(fd) {
    // 1. set size
    size_ = align_to_page(size);
    // 2. map memory
    map_memory();
}

Shmem::Shmem(Shmem&& other) noexcept {
    move_from(std::move(other));
}

Shmem& Shmem::operator=(Shmem&& other) noexcept {
    if (this != &other) {
        cleanup();
        move_from(std::move(other));
    }
    return *this;
}

} // namespace ipc
} // namespace ugdr
#pragma once
#include <stdexcept>
#include <string>
#include <cstring>

namespace ugdr{

namespace core{
class Ctx;
}

namespace ipc{

class Shmem {
public:
    // for daemon create mem_fd
    Shmem(std::string name, size_t size);
    // for client receive mem_fd
    Shmem(std::string name, size_t size, int fd);
    virtual ~Shmem() = default;
    Shmem(const Shmem&) = delete;
    Shmem& operator=(const Shmem&) = delete;
    Shmem(Shmem&& other) noexcept;
    Shmem& operator=(Shmem&& other) noexcept;

    //basic info
    inline std::string get_name() const { return name_; }
    inline size_t get_size() const { return size_; }
    inline int get_fd() const { return fd_; }

    //TODO: exprimental read/write methods, need delete later
    void write(const void* data, size_t size){
        if (size > size_) {
            throw_error(name_ + " write size exceeds shm size");
        }
        ::memcpy(addr_, data, size);
    }
    void read(void* data, size_t size){
        if (size > size_) {
            throw_error(name_ + " read size exceeds shm size");
        }
        ::memcpy(data, addr_, size);
    }
protected:
    inline void throw_error(const std::string& msg){
        throw std::runtime_error("Shmem error: " + msg);
    }
private:
    void map_memory();
    void cleanup();
    void move_from(Shmem&& other) noexcept;

    std::string name_;
    int fd_ = -1;
    size_t size_ = 0;
    void* addr_ = nullptr;
};

class Shring : public Shmem {
public:
    Shring(std::string name, size_t size) : Shmem(name, size) {}
    virtual ~Shring() = default;
private:
};

} // namespace ipc
} // namespace ugdr
#pragma once
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdexcept>
#include <string>
#include <cstring>
#include <cerrno>
#include "../../common/logger.h"

namespace ugdr{
namespace ipc{

const std::string UDS_PATH_DEFAULT = "/tmp/ugdr.sock";

class Socket {
public:
    Socket(): fd_(-1) {}
    explicit Socket(int fd): fd_(fd) {}
    ~Socket(){
        close_fd();
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) : fd_(other.fd_) {
        other.fd_ = -1;
    }
    Socket& operator=(Socket&& other) noexcept {
        if(this != &other){
            close_fd();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    inline int get_fd() const { return fd_; }

    void close_fd(){
        if(fd_ >= 0){
            ::close(fd_);
            fd_ = -1;
        }
    }
private:
    int fd_;
};

inline Socket create_server_socket(const std::string& path){
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket() failed: " + std::string(strerror(errno)));

    Socket sock(fd);
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    ::unlink(path.c_str());
    if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        throw std::runtime_error("bind() failed: " + std::string(strerror(errno)));
    }

    if (::listen(fd, 128) < 0) {
        throw std::runtime_error("listen() failed " + std::string(strerror(errno)));
    }

    return sock;
}

inline Socket connect_to_server(const std::string& path){
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket() failed: " + std::string(strerror(errno)));

    Socket sock(fd);
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        throw std::runtime_error("connect() failed: " + std::string(strerror(errno)));
        return Socket(-1);
    }

    return sock;
}

}
}
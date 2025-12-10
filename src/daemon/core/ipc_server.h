#pragma once
#include <string>
#include <vector>
#include "../../common/ipc/socket_utils.h"

namespace ugdr{
namespace core{

class Manager;

class IpcServer {
public:
    IpcServer(std::string uds_path, Manager* manager);
    ~IpcServer();

    void run_loop();

private:
    void set_non_blocking(int fd);
    void handle_new_connect();
    bool handle_client_msg(int client_fd);

    std::string uds_path_;
    Manager* manager_;
    ipc::Socket server_socket_;
    ipc::Socket epoll_fd_;
    std::vector<struct epoll_event> events_;
    bool running_ = false;
};

}
}
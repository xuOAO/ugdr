#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "../../common/ipc/socket_utils.h"
#include "../../common/ipc/ipc_proto.h"

namespace ugdr{
namespace core{

class Manager;
class Ctx;

class IpcServer {
public:
    IpcServer(std::string uds_path, Manager* manager);
    ~IpcServer();

    void run_loop();

private:
    using CmdHandler = bool(*)(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp);

    void set_non_blocking(int fd);
    void handle_new_connect();
    bool handle_client_msg(int client_fd);
    void cleanup_client(int client_fd);

    static bool handleOpenDevice(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp);
    static bool handleCloseDevice(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp);
    static bool handleUnknownCmd(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp);
    CmdHandler cmdToHandler(ipc::Cmd cmd);

    std::string uds_path_;
    Manager* manager_;
    std::unordered_map<int, Ctx*> client_ctx_map_;

    ipc::Socket server_socket_;
    ipc::Socket epoll_fd_;
    bool running_ = false;
};

}
}
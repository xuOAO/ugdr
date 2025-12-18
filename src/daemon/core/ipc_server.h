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
class Pd;

class IpcServer {
public:
    IpcServer(std::string uds_path, Manager* manager);
    ~IpcServer();

    void run_loop();

    inline Ctx* get_ctx(int client_fd);

private:
    using CmdHandler = int(*)(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp);

    void set_non_blocking(int fd);
    void handle_new_connect();
    bool handle_client_msg(int client_fd);
    void cleanup_client(int client_fd);

    constexpr static int NORMAL_SEND = -1;
    constexpr static int CLOSE_SOCK = -2;
    constexpr static int NO_OPERATION = -3;

    static int handleOpenDevice(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp);
    static int handleCloseDevice(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp);
    static int handleAllocPd(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp);
    static int handleDeallocPd(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp);
    static int handleCreateCq(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp);
    static int handleDestroyCq(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp);
    static int handleCreateQp(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp);
    static int handleDestroyQp(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp);
    static int handleUnknownCmd(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp);
    //experimental cmd handler can be added here
    static int handleExperimental(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp);
    CmdHandler cmdToHandler(ipc::Cmd cmd);

    std::string uds_path_;
    Manager* manager_;
    std::unordered_map<int, Ctx*> client_ctx_map_; // key: client_fd, value: context pointer

    ipc::Socket server_socket_;
    ipc::Socket epoll_fd_;
    bool running_ = false;
};

}
}
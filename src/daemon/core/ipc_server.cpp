#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include "ipc_server.h"
#include "../../common/logger.h"
#include "manager.h"

namespace ugdr{
namespace core{

constexpr int MAX_EVENTS = 128;

IpcServer::IpcServer(std::string uds_path, Manager* manager)
    : uds_path_(uds_path), manager_(manager) {
}

IpcServer::~IpcServer() = default;

void IpcServer::set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        UGDR_LOG_INFO("fcntl F_GETFL failed: %s", strerror(errno));
        return;
    }
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void IpcServer::handle_new_connect() {
    struct sockaddr_un client_addr;
    socklen_t len = sizeof(client_addr);

    int client_fd = ::accept(server_socket_.get_fd(), (struct sockaddr*)&client_addr, &len);
    if (client_fd < 0) {
        UGDR_LOG_INFO("accept failed: %s", strerror(errno));
        return;
    }

    UGDR_LOG_INFO("[Server]: New client connected: %d", client_fd);

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = client_fd;
    if(::epoll_ctl(epoll_fd_.get_fd(), EPOLL_CTL_ADD, client_fd, &ev) < 0){
        UGDR_LOG_INFO("epoll_ctl failed: %s", strerror(errno));
        ::close(client_fd);
        return;
    }
}

bool IpcServer::handleOpenDevice(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp){
    int status = 0;
    bool ret = true;
    Ctx* ctx;

    // 1.handle
    ctx = server->manager_->get_ctx(std::string(req.open_dev_req.dev_name));
    if (ctx == nullptr){
        UGDR_LOG_INFO("[Server]: client %d open device failed, unknown device name: %s", client_fd, req.open_dev_req.dev_name);
        status = -1;
        ret = false;
    } else{
        server->client_ctx_map_[client_fd] = ctx;
    }

    // 2.build response
    rsp = {
        .header = {
            .magic = ipc::UGDR_PROTO_MAGIC,
            .cmd = ipc::Cmd::UGDR_CMD_OPEN_DEVICE,
            .status = status,
        },
        .open_dev_rsp = {
        },
    };

    UGDR_LOG_INFO("[Server]: client %d open device %s, status= %d", client_fd, req.open_dev_req.dev_name, status);
    return ret;
}

bool IpcServer::handleCloseDevice(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp){
    int status = 0;
    bool ret = false; // 默认返回false，断开连接

    // 直接返回false，断开连接
    rsp = {
        .header = {
            .magic = ipc::UGDR_PROTO_MAGIC,
            .cmd = ipc::Cmd::UGDR_CMD_CLOSE_DEVICE,
            .status = status,
        },
    };

    UGDR_LOG_INFO("[Server]: client %d close device", client_fd);
    return ret;
}

bool IpcServer::handleUnknownCmd(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp){
    UGDR_LOG_INFO("[Server]: Unknown cmd: %d", static_cast<uint32_t>(req.header.cmd));
    return true;
}

IpcServer::CmdHandler IpcServer::cmdToHandler(ipc::Cmd cmd) {
    switch(cmd) {
        case ipc::Cmd::UGDR_CMD_OPEN_DEVICE: return handleOpenDevice;
        case ipc::Cmd::UGDR_CMD_CLOSE_DEVICE: return handleCloseDevice;
        default: return handleUnknownCmd;
    }
}

bool IpcServer::handle_client_msg(int client_fd) {
    try{
        bool ret = true;
        ssize_t n = 0;
        struct ipc::ugdr_request req;
        struct ipc::ugdr_response rsp;

        // 1.recv request
        n = ::recv(client_fd, &req, sizeof(req), MSG_WAITALL);
        //TODO:数据包破碎，暂不处理
        if (n < sizeof(req)) throw std::runtime_error("Failed to receive request from client");

        // 2.handle request and produce response
        if (req.header.magic != ipc::UGDR_PROTO_MAGIC) return false;
        CmdHandler handler = cmdToHandler(req.header.cmd);
        ret = handler(this, client_fd, req, rsp);

        // 3.send response
        n = ::send(client_fd, &rsp, sizeof(rsp), MSG_WAITALL);
        if (n < sizeof(rsp)) throw std::runtime_error("Failed to send response to client");

        return ret;
    } catch(const std::exception& e){
        UGDR_LOG_ERROR("[Server]: Error: %s", e.what());
        return false;
    }
}

void IpcServer::cleanup_client(int client_fd) {
    UGDR_LOG_DEBUG("[Server]: Cleaning up client fd: %d", client_fd);
    // 1. 从 epoll 实例中删除
    if (::epoll_ctl(epoll_fd_.get_fd(), EPOLL_CTL_DEL, client_fd, nullptr) < 0) {
        UGDR_LOG_ERROR("[Server]: epoll_ctl(DEL) failed for fd %d: %s", client_fd, strerror(errno));
    }

    // 2. 关闭文件描述符
    ::close(client_fd);

    // 3. 从客户上下文字典中删除
    if (client_ctx_map_.erase(client_fd) > 0) {
        UGDR_LOG_DEBUG("[Server]: Cleaned up context for disconnected client fd: %d", client_fd);
    }
}

void IpcServer::run_loop() {
    // 1.创建server对象并设置为非阻塞
    server_socket_ = ipc::create_server_socket(uds_path_);
    set_non_blocking(server_socket_.get_fd());

    // 2.创建epoll实例
    int epoll_raw = ::epoll_create1(0);
    if(epoll_raw < 0) throw std::runtime_error("epoll_create1 failed");
    epoll_fd_ = ipc::Socket(epoll_raw);

    // 3.将server socket加入epoll监听
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_socket_.get_fd();
    if(::epoll_ctl(epoll_fd_.get_fd(), EPOLL_CTL_ADD, server_socket_.get_fd(), &ev) < 0){
        throw std::runtime_error("epoll_ctl failed for server socket");
    }

    // 4. 创建一个固定的、用于接收epoll_wait结果的数组
    struct epoll_event triggered_events[MAX_EVENTS];

    running_ = true;
    while(running_) {
        int n = epoll_wait(epoll_fd_.get_fd(), triggered_events, MAX_EVENTS, -1);
        if (n < 0){
            if (errno == EINTR) continue;
            UGDR_LOG_ERROR("[Server]: epoll_wait failed: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < n; ++i) {
            struct epoll_event& current_event = triggered_events[i];
            int current_fd = current_event.data.fd;

            if (current_fd == server_socket_.get_fd()){
                handle_new_connect();
            } else {
                // 检查错误或断开连接事件
                if ((current_event.events & EPOLLERR) || (current_event.events & EPOLLHUP)) {
                    UGDR_LOG_INFO("[Server]: Client fd %d disconnected or encountered an error.", current_fd);
                    cleanup_client(current_fd);
                    continue;
                }

                // 处理可读事件
                if (current_event.events & EPOLLIN) {
                    if (!handle_client_msg(current_fd)){
                        // 业务逻辑要求断开连接 (例如 handleCloseDevice)
                        cleanup_client(current_fd);
                    }
                }
            }
        }
    }
}

}
}
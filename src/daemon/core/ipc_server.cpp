#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "ctx.h"
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

Ctx* IpcServer::get_ctx(int client_fd) {
    auto it = client_ctx_map_.find(client_fd);
    if (it == client_ctx_map_.end()) {
        return nullptr;
    }
    return it->second;
}

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

int IpcServer::handleOpenDevice(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp){
    int status = 0;
    int ret = NORMAL_SEND;
    Ctx* ctx;

    // 1.handle
    ctx = server->manager_->get_ctx(std::string(req.open_dev_req.dev_name));
    if (ctx == nullptr){
        UGDR_LOG_INFO("[Server]: client %d open device failed, unknown device name: %s", client_fd, req.open_dev_req.dev_name);
        status = -1;
        ret = CLOSE_SOCK;
    } else{
        server->client_ctx_map_[client_fd] = ctx;
    }

    // 2.build response
    rsp = ipc::ugdr_response{
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

int IpcServer::handleCloseDevice(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp){
    int status = 0;
    int ret = CLOSE_SOCK; // 默认返回false，断开连接

    // 直接返回false，断开连接
    rsp = ipc::ugdr_response{
        .header = {
            .magic = ipc::UGDR_PROTO_MAGIC,
            .cmd = ipc::Cmd::UGDR_CMD_CLOSE_DEVICE,
            .status = status,
        },
    };

    UGDR_LOG_INFO("[Server]: client %d close device", client_fd);
    return ret;
}

int IpcServer::handleAllocPd(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp){
    int status = 0;
    uint32_t pd_handle = 0;
    int ret = NORMAL_SEND;
    Ctx* ctx;

    // 1.handle
    ctx = server->get_ctx(client_fd);
    if(ctx == nullptr) {
        UGDR_LOG_INFO("[Server]: client %d alloc pd failed, no context", client_fd);
        status = -1;
        ret = CLOSE_SOCK;
    } else {
        pd_handle = ctx->alloc_pd(); 
    }

    // 2.build response
    rsp = ipc::ugdr_response{
        .header = {
            .magic = ipc::UGDR_PROTO_MAGIC,
            .cmd = ipc::Cmd::UGDR_CMD_ALLOC_PD,
            .status = status,
        },
        .alloc_pd_rsp = {
            .pd_handle = pd_handle,
        },
    };

    UGDR_LOG_INFO("[Server]: client %d alloc pd, status= %d, pd_handle= %u", client_fd, status, pd_handle);
    return ret;
}

int IpcServer::handleDeallocPd(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp){
    int status = 0;
    int ret = NORMAL_SEND;
    uint32_t pd_handle = 0;
    Ctx* ctx;

    // 1.handle
    ctx = server->get_ctx(client_fd);
    if (ctx == nullptr) {
        UGDR_LOG_INFO("[Server]: client %d dealloc pd failed, no context", client_fd);
        status = -1;
        ret = CLOSE_SOCK;
    } else {
        pd_handle = req.destroy_rsrc_req.handle.pd_handle;
        if (ctx->dealloc_pd(pd_handle) != 0){
            UGDR_LOG_INFO("[Server]: client %d dealloc pd failed, invalid pd_handle= %u", client_fd, pd_handle);
            status = -1;
            ret = CLOSE_SOCK;
        }

    }

    // 2.build response
    rsp = ipc::ugdr_response{
        .header = {
            .magic = ipc::UGDR_PROTO_MAGIC,
            .cmd = ipc::Cmd::UGDR_CMD_DEALLOC_PD,
            .status = status,
        },
    };

    UGDR_LOG_INFO("[Server]: client %d dealloc pd = %d, status= %d", client_fd, pd_handle, status);
    return ret;
}

int IpcServer::handleCreateCq(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp){
    int status = 0;
    int ret = NO_OPERATION;
    uint32_t cq_handle = 0;
    ssize_t n = 0;
    Ctx* ctx = nullptr;
    //TODO: 未来需要替换成 ShmRing 类
    struct shmring_attr shmring_attr;

    // 1.handle
    ctx = server->get_ctx(client_fd);
    if (ctx == nullptr) {
        UGDR_LOG_INFO("[Server]: client %d create cq failed, no context", client_fd);
        status = -1;
        ret = CLOSE_SOCK;
    } else {
        cq_handle = ctx->create_cq(req.create_cq_req.cqe, &shmring_attr);
        //TODO: 未来需要替换成 ShmRing 类
        if (shmring_attr.fd < 0) {
            UGDR_LOG_INFO("[Server]: client %d create cq failed", client_fd);
            status = -1;
            ret = CLOSE_SOCK;
        }

    }

    // 2.build response
    rsp = ipc::ugdr_response{
        .header = {
            .magic = ipc::UGDR_PROTO_MAGIC,
            .cmd = ipc::Cmd::UGDR_CMD_CREATE_CQ,
            .status = status,
        },
        .create_cq_rsp = {
            //TODO: 未来需要替换成 ShmRing 类
            .shring_size = shmring_attr.ring_size, 
            .cq_handle = cq_handle,
        },
    };
    strncpy(rsp.create_cq_rsp.shring_name, shmring_attr.ring_name.c_str(), sizeof(rsp.create_cq_rsp.shring_name)-1);

    // 3.send response with fd
    std::vector<int> fds = {shmring_attr.fd};
    if (ret == NO_OPERATION) {
        n = ipc::send_rsp_with_fds(client_fd, &rsp, sizeof(rsp), fds);
        if (n < sizeof(rsp)) {
            ctx->destroy_cq(cq_handle);
            throw std::runtime_error("Failed to send create_cq response with fd to client");
        }
    }

    UGDR_LOG_INFO("[Server]: client %d create cq, status= %d, cq_handle= %u", client_fd, status, cq_handle);
    return ret;
}

int IpcServer::handleDestroyCq(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp){ 
    int status = 0;
    int ret = NORMAL_SEND;
    Ctx* ctx = nullptr;

    // 1.handle
    ctx = server->get_ctx(client_fd);
    if (ctx == nullptr) {
        UGDR_LOG_INFO("[Server]: client %d destroy cq failed, no context", client_fd);
        status = -1;
        ret = CLOSE_SOCK;
    } else {
        if (ctx->destroy_cq(req.destroy_rsrc_req.handle.cq_handle) != 0) {
            throw std::runtime_error("Failed to destroy cq");
        }
    }

    // 2.build response
    rsp = ipc::ugdr_response{
        .header = {
            .magic = ipc::UGDR_PROTO_MAGIC,
            .cmd = ipc::Cmd::UGDR_CMD_DESTROY_CQ,
            .status = status,
        },
    };

    UGDR_LOG_INFO("[Server]: client %d destroy cq, status= %d", client_fd, status);
    return ret;
}

int IpcServer::handleCreateQp(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp){ 
    int status = 0;
    int ret = NO_OPERATION;
    ssize_t n = 0;
    uint32_t qp_handle = 0;
    struct shmring_attr sq_attr;
    struct shmring_attr rq_attr;
    ipc::Shmem* send_cq = nullptr;
    ipc::Shmem* recv_cq = nullptr;
    Ctx* ctx = nullptr;
    Pd* pd = nullptr;

    // 1.handle
    ctx = server->get_ctx(client_fd);
    if (ctx == nullptr) {
        UGDR_LOG_INFO("[Server]: client %d create qp failed, no context", client_fd);
        status = -1;
        ret = CLOSE_SOCK;
    } else {
        pd = ctx->get_pd(req.create_qp_req.pd_handle);
        if (pd == nullptr) {
            ipc::Shmem* recv_cq = nullptr;
            UGDR_LOG_INFO("[Server]: client %d create qp failed, no pd", client_fd);
            status = -1;
            ret = CLOSE_SOCK;
        } else {
            send_cq = ctx->get_cq(req.create_qp_req.qp_attr.send_cq_handle);
            recv_cq = ctx->get_cq(req.create_qp_req.qp_attr.recv_cq_handle);
            struct qp_init_attr qp_init_attr = {
                .send_cq = send_cq,
                .recv_cq = recv_cq,
                .max_send_wr = req.create_qp_req.qp_attr.cap.max_send_wr,
                .max_recv_wr = req.create_qp_req.qp_attr.cap.max_recv_wr,
                .qp_type = req.create_qp_req.qp_attr.qp_type,
                .sq_sig_all = req.create_qp_req.qp_attr.sq_sig_all
            };
            qp_handle = pd->create_qp(qp_init_attr, &sq_attr, &rq_attr);
            if (rq_attr.fd < 0 || sq_attr.fd < 0) {
                UGDR_LOG_INFO("[Server]: client %d create qp failed rq_fd = %d, sq_fd = %d", rq_attr.fd, sq_attr.fd);
                status = -1;
                ret = CLOSE_SOCK;
            }
        }
    }

    // 2.build response
    rsp = ipc::ugdr_response{
        .header = {
            .magic = ipc::UGDR_PROTO_MAGIC,
            .cmd = ipc::Cmd::UGDR_CMD_CREATE_QP,
            .status = status,
        },
        .create_qp_rsp = {
            .qp_handle = qp_handle,
            .sq_size = sq_attr.ring_size,
            .rq_size = rq_attr.ring_size,
        },
    };
    strncpy(rsp.create_qp_rsp.sq_name, sq_attr.ring_name.c_str(), sizeof(rsp.create_qp_rsp.sq_name)-1);
    strncpy(rsp.create_qp_rsp.rq_name, rq_attr.ring_name.c_str(), sizeof(rsp.create_qp_rsp.rq_name)-1);

    // 3.send response with fds
    if (ret == NO_OPERATION) {
        std::vector<int> fds = {rq_attr.fd, sq_attr.fd};
        n = ipc::send_rsp_with_fds(client_fd, &rsp, sizeof(rsp), fds);
        if (n < sizeof(rsp)) {
            pd->destroy_qp(qp_handle);
            throw std::runtime_error("Failed to send create_qp response with fds to client");
        }
    }

    UGDR_LOG_INFO("[Server]: client %d create qp, status= %d, qp_handle= %u", client_fd, status, qp_handle);

    return ret;
}

int IpcServer::handleDestroyQp(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp){
    int status = 0;
    int ret = NORMAL_SEND;
    Ctx* ctx = nullptr;
    Pd* pd = nullptr;

    // 1.handle
    ctx = server->get_ctx(client_fd);
    if (ctx == nullptr) {
        UGDR_LOG_INFO("[Server]: client %d destroy qp failed, no context", client_fd);
        status = -1;
        ret = CLOSE_SOCK;
    } else {
        pd = ctx->get_pd(req.destroy_qp_req.pd_handle);
        if (pd == nullptr) {
            UGDR_LOG_INFO("[Server]: client %d destroy qp failed, no pd", client_fd);
            status = -1;
            ret = CLOSE_SOCK;
        } else {
            if (pd->destroy_qp(req.destroy_qp_req.qp_handle) != 0) {
                throw std::runtime_error("Failed to destroy qp");
            }

        }
    }
    // 2.build response
    rsp = ipc::ugdr_response{
        .header = {
            .magic = ipc::UGDR_PROTO_MAGIC,
            .cmd = ipc::Cmd::UGDR_CMD_DESTROY_QP,
            .status = status,
        },
    };

    UGDR_LOG_INFO("[Server]: client %d destroy qp, status= %d", client_fd, status);

    return ret;
}

int IpcServer::handleUnknownCmd(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp){
    UGDR_LOG_INFO("[Server]: Unknown cmd: %d", static_cast<uint32_t>(req.header.cmd));
    rsp = ipc::ugdr_response{
        .header = {
            .magic = ipc::UGDR_PROTO_MAGIC,
            .cmd = req.header.cmd,
            .status = -1,
        },
    };
    return CLOSE_SOCK;
}

int IpcServer::handleExperimental(IpcServer* server, int client_fd, struct ipc::ugdr_request& req, struct ipc::ugdr_response& rsp){
    int status = 0;
    int ret = NORMAL_SEND;
    int data = 0;
    Ctx* ctx = nullptr;
    Pd* pd = nullptr;
    Qp* qp = nullptr;
    ipc::Shmem* shm = nullptr;

    // 1.handle
    ctx = server->get_ctx(client_fd);
    if (ctx == nullptr) {
        UGDR_LOG_INFO("[Server]: client %d experimental cmd failed, no context", client_fd);
        status = -1;
        ret = CLOSE_SOCK;
    } else {
        // read data from Shmem
        switch (req.experimental_req.type) {
        case 0:
            shm = ctx->get_cq(static_cast<uint32_t>(req.experimental_req.cq.cq_handle));
            break;
        case 1:
            pd = ctx->get_pd(req.experimental_req.qp.pd_handle);
            qp = pd->get_qp(req.experimental_req.qp.qp_handle); 
            shm = qp->get_sq();
            break;
        case 2:
            pd = ctx->get_pd(req.experimental_req.qp.pd_handle);
            qp = pd->get_qp(req.experimental_req.qp.qp_handle);
            shm = qp->get_rq();
            break;
        }
        shm->read(&data, sizeof(data));
    }

    // 2.build response
    rsp = ipc::ugdr_response{
        .header = {
            .magic = ipc::UGDR_PROTO_MAGIC,
            .cmd = ipc::Cmd::UGDR_CMD_EXPERIMENTAL,
            .status = status,
        },
        .experimental_rsp = {
            .data = data,
        },
    };

    UGDR_LOG_INFO("[Server]: client %d experimental cmd, status= %d, data= %d", client_fd, status, data);
    return ret;
}

IpcServer::CmdHandler IpcServer::cmdToHandler(ipc::Cmd cmd) {
    switch(cmd) {
        case ipc::Cmd::UGDR_CMD_OPEN_DEVICE: return handleOpenDevice;
        case ipc::Cmd::UGDR_CMD_CLOSE_DEVICE: return handleCloseDevice;
        case ipc::Cmd::UGDR_CMD_ALLOC_PD: return handleAllocPd;
        case ipc::Cmd::UGDR_CMD_DEALLOC_PD: return handleDeallocPd;
        case ipc::Cmd::UGDR_CMD_CREATE_CQ: return handleCreateCq;
        case ipc::Cmd::UGDR_CMD_DESTROY_CQ: return handleDestroyCq;
        case ipc::Cmd::UGDR_CMD_CREATE_QP: return handleCreateQp;
        case ipc::Cmd::UGDR_CMD_DESTROY_QP: return handleDestroyQp;
        //experimental cmds
        case ipc::Cmd::UGDR_CMD_EXPERIMENTAL: return handleExperimental;
        default: return handleUnknownCmd;
    }
}

bool IpcServer::handle_client_msg(int client_fd) {
    try{
        bool ret = true;
        ssize_t n = 0;
        int operation = NO_OPERATION;
        struct ipc::ugdr_request req;
        struct ipc::ugdr_response rsp;

        // 1.recv request
        n = ::recv(client_fd, &req, sizeof(req), MSG_WAITALL);
        //TODO:数据包破碎，暂不处理
        if (n < sizeof(req)) throw std::runtime_error("Failed to receive request from client");

        // 2.handle request and produce response
        if (req.header.magic != ipc::UGDR_PROTO_MAGIC) return false;
        CmdHandler handler = cmdToHandler(req.header.cmd);
        operation = handler(this, client_fd, req, rsp);

        // 3.do post operation
        switch (operation) {
            case NORMAL_SEND:
                // 正常发送响应
                n = ::send(client_fd, &rsp, sizeof(rsp), MSG_WAITALL);
                if (n < sizeof(rsp)) throw std::runtime_error("Failed to send response to client");
                ret = true;
                break;
            case CLOSE_SOCK:
                n = ::send(client_fd, &rsp, sizeof(rsp), MSG_WAITALL);
                if (n < sizeof(rsp)) throw std::runtime_error("Failed to send response to client");
                // 关闭连接
                ret = false;
                break;
            case NO_OPERATION:
                // 不进行任何操作
                ret = true;
                break;
            default:
                ret = false;
                break;
        }

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
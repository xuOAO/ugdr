#include <bits/stdint-uintn.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include "ipc_server.h"
#include "../../common/logger.h"
#include "../../common/ipc/ipc_proto.h"
#include "manager.h"

namespace ugdr{
namespace core{

constexpr int MAX_EVENTS = 128;

IpcServer::IpcServer(std::string uds_path, Manager* manager)
    : uds_path_(uds_path), manager_(manager) {
    events_.reserve(MAX_EVENTS);
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
    events_.push_back(ev);
}

bool IpcServer::handle_client_msg(int client_fd) {
//TODO:handle的输出可以考虑格式化后，单独写一个inline函数
    try{
        struct ipc::Header header;

        //TODO:MSG_WAITALL可能存在问题，需调研
        ssize_t n = ::recv(client_fd, &header, sizeof(header), MSG_WAITALL);

        if (n == 0) return false;
        if (n < 0) return false;
        //TODO:数据包破碎，暂不处理
        if (n != sizeof(header)) return true;

        if (header.magic != ipc::UGDR_PROTO_MAGIC) return false;

        switch (header.cmd) {
            case ipc::Cmd::UGDR_CMD_INIT:{
                //返回ctx
                ipc::DeviceName payload;
                //TODO:MSG_WAITALL可能存在问题，需调研
                ::recv(client_fd, &payload, sizeof(payload), MSG_WAITALL);
                UGDR_LOG_INFO("[Server]: RECV_CMD: %s, eth_name= %s", ipc::CmdStr[static_cast<int>(header.cmd)], payload.name);

                std::string dev_name(payload.name);
                //TODO: 补充错误处理逻辑
                uint32_t ctx_idx = manager_->get_ctx(dev_name);
                struct ipc::InitRsp rsp = {
                    .header = {
                        .cmd = ipc::Cmd::UGDR_CMD_RESP,
                        .payload_len = sizeof(rsp.ctx_idx),
                    },
                    .ctx_idx = ctx_idx,
                };
                //TODO:MSG_WAITALL可能存在问题，需调研
                ::send(client_fd, &rsp, sizeof(rsp), MSG_WAITALL);
                UGDR_LOG_INFO("[Server]: SEND_CMD: %s, ctx_idx= %d", ipc::CmdStr[static_cast<int>(rsp.header.cmd)], ctx_idx);

                break;
            }    
            case ipc::Cmd::UGDR_CMD_EXIT:{
                UGDR_LOG_INFO("[Server]: RECV_CMD: %s", ipc::CmdStr[static_cast<int>(header.cmd)]);
                //直接返回false，断开连接
                ipc::ExitRsp rsp = {
                    .header = {
                        .cmd = ipc::Cmd::UGDR_CMD_RESP,
                        .payload_len = 0,
                    },
                };
                //TODO:MSG_WAITALL可能存在问题，需调研
                ::send(client_fd, &rsp, sizeof(rsp), MSG_WAITALL);
                UGDR_LOG_INFO("[Server]: SEND_CMD: %s", ipc::CmdStr[static_cast<int>(rsp.header.cmd)]);
                
                return false;
                break;
            }
            // case ipc::Cmd::UGDR_CMD_ALLOC_PD:{
            //     uint32_t ctx_idx = 0;
            //     //TODO: MSG_WAITALL可能存在问题，需调研
            //     ::recv(client_fd, &ctx_idx, sizeof(ctx_idx), MSG_WAITALL);
            //     //TODO:补充错误处理逻辑
            //     uint32_t pd_idx = manager_->alloc_pd(ctx_idx);
            //     struct ipc::AllocPdRsp rsp = {
            //         .header = {
            //             .cmd = ipc::Cmd::UGDR_CMD_ALLOC_PD,
            //             .payload_len = sizeof(rsp.pd_idx),
            //         },
            //         .pd_idx = pd_idx,
            //     };
            //     //TODO:MSG_WAITALL可能存在问题，需调研
            //     ::send(client_fd, &rsp, sizeof(rsp), MSG_WAITALL);
            //     UGDR_LOG_INFO("[Server]: SEND_CMD: %s, pd_idx= %d", ipc::CmdStr[static_cast<int>(rsp.header.cmd)], pd_idx);

            //     break;
            // }
            // case ipc::Cmd::UGDR_CMD_DEALLOC_PD:{
            //     int ret = 0;
            //     struct ipc::PdReqPayload payload;
            //     //TODO: MSG_WAITALL可能存在问题，需调研
            //     ::recv(client_fd, &payload, sizeof(payload), MSG_WAITALL);
            //     //TODO:补充错误处理逻辑
            //     ret = manager_->dealloc_pd(payload.ctx_idx, payload.pd_idx);
            //     if (ret != 0) {
            //         UGDR_LOG_INFO("[Server]: dealloc_pd failed: %s", strerror(ret));
            //     }
            //     struct ipc::DeallocPdRsp rsp = {
            //         .header = {
            //             .cmd = ipc::Cmd::UGDR_CMD_DEALLOC_PD,
            //             .payload = sizeof(rsp.ret),
            //         }
            //         .ret = ret,
            //     };
            //     //TODO:MSG_WAITALL可能存在问题，需调研
            //     ::send(client_fd, &rsp, sizeof(rsp), MSG_WAITALL);
            //     UGDR_LOG_INFO("[Server]: SEND_CMD: %s, ret= %d", ipc::CmdStr[static_cast<int>(rsp.header.cmd)], ret);

            //     break;
            // }
            default:
                UGDR_LOG_INFO("[Server]: Unknown cmd: %d", header.cmd);
                break;
        }
    }
    catch(const std::exception& e){
        UGDR_LOG_ERROR("[Server]: Error: %s", e.what());
        return false;
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
        throw std::runtime_error("epoll_ctl failed");
    }

    running_ = true;
    events_.push_back(ev);

    while(running_) {
        int n = epoll_wait(epoll_fd_.get_fd(), events_.data(), events_.size(), -1);
        if (n < 0){
            if (errno == EINTR) continue;
            break;
        }

        for (struct epoll_event& ev : events_) {
            if (ev.data.fd == server_socket_.get_fd()){
                handle_new_connect();
            }
            else{
                if (!handle_client_msg(ev.data.fd)){
                    ::close(ev.data.fd);
                }
            }
        }
    }
}

}
}
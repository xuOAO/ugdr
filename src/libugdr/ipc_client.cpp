#include <sys/socket.h>
#include "ipc_client.h"
#include "../common/ipc/ipc_proto.h"
#include "../common/logger.h"

namespace ugdr{
namespace lib{

struct ::ugdr_context IpcClient::connect_and_handshake(const std::string& dev_name){
    ugdr::ipc::Socket sock = ugdr::ipc::connect_to_server(ugdr::ipc::UDS_PATH_DEFAULT);
    if(sock.get_fd() == -1){
        throw std::runtime_error("Faild to connect to server");
    }

    ipc::InitReq init_req;
    init_req.header.cmd = ipc::Cmd::UGDR_CMD_INIT;
    init_req.header.payload_len = sizeof(ipc::DeviceName);
    std::strncpy(init_req.device_name.name, dev_name.c_str(), sizeof(init_req.device_name.name));

    //TODO:MSG_WAITALL可能存在问题，需调研
    ::send(sock.get_fd(), &init_req, sizeof(init_req), MSG_WAITALL);

    ipc::InitRsp init_rsp;
    //TODO:MSG_WAITALL可能存在问题，需调研
    ::recv(sock.get_fd(), &init_rsp, sizeof(init_rsp), MSG_WAITALL);

    if(init_rsp.header.magic != ipc::UGDR_PROTO_MAGIC){
        throw std::runtime_error("Invalid magic number in init response");
    }

    if(init_rsp.header.cmd != ipc::Cmd::UGDR_CMD_RESP){
        //TODO: 更多的处理逻辑
        switch(init_rsp.header.cmd){
            case ipc::Cmd::UGDR_CMD_ERR:
                throw std::runtime_error("Server error in init response");
            default:
                throw std::runtime_error("Invalid cmd in init response");
        }
    }

    return ugdr_context{std::move(sock), init_rsp.ctx_idx};
}

bool IpcClient::close(struct ugdr_context* ctx){
    //TODO: 对于nullptr的表现暂定
    if (ctx == nullptr) return true;

    ipc::ExitReq exit_req;
    exit_req.header.cmd = ipc::Cmd::UGDR_CMD_EXIT;
    exit_req.header.payload_len = 0;

    //TODO:MSG_WAITALL可能存在问题，需调研
    ::send(ctx->sock.get_fd(), &exit_req, sizeof(exit_req), MSG_WAITALL);

    ipc::InitRsp init_rsp;
    //TODO:MSG_WAITALL可能存在问题，需调研
    ::recv(ctx->sock.get_fd(), &init_rsp, sizeof(init_rsp), MSG_WAITALL);

    if(init_rsp.header.magic != ipc::UGDR_PROTO_MAGIC){
        throw std::runtime_error("Invalid magic number in exit response");
    }

    if(init_rsp.header.cmd != ipc::Cmd::UGDR_CMD_RESP){
        //TODO: 更多的处理逻辑
        switch(init_rsp.header.cmd){
            case ipc::Cmd::UGDR_CMD_ERR:
                throw std::runtime_error("Server error in exit response");
            default:
                UGDR_LOG_INFO("cmd : %d", init_rsp.header.cmd);
                throw std::runtime_error("Invalid cmd in exit response");
        }
    }

    return true;
}
}
}
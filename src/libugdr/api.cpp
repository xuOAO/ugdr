#include "../../include/ugdr.h"
#include "../common/logger.h"
#include "../common/ipc/ipc_proto.h"
#include "ugdr_internal.h"
#include "ipc_client.h"

struct ugdr_context* ugdr_open_device(const char* dev_name){
    if (dev_name == nullptr) return nullptr;
    try {
        struct ugdr_context* ctx = new ugdr_context();
        if (ctx){
            // 1. prehandle
            struct ugdr::ipc::ugdr_request req;
            struct ugdr::ipc::ugdr_response rsp;

            std::strncpy(ctx->dev_name, dev_name, sizeof(ctx->dev_name));
            ctx->sock = ugdr::ipc::connect_to_server(ugdr::ipc::UDS_PATH_DEFAULT);

            // 2. build request
            req = {
                .header = {
                    .magic = ugdr::ipc::UGDR_PROTO_MAGIC,
                    .cmd = ugdr::ipc::Cmd::UGDR_CMD_OPEN_DEVICE,
                    .status = 0,
                },
                .open_dev_req = {
                    .dev_name = {},
                },
            };
            std::strncpy(req.open_dev_req.dev_name, dev_name, sizeof(req.open_dev_req.dev_name));

            // 3. send req and handle rsp
            ugdr::lib::IpcClient::sendReqAndHandleRsp(ctx->sock.get_fd(), req, rsp);

            // 4. posthandle: open_device do nothing here
            return ctx;
        } else{
            throw std::runtime_error("No memory to create context");
        }
    } catch(const std::exception& e){
        UGDR_LOG_ERROR("[Client]: Failed to open device %s: %s", dev_name, e.what());
        return nullptr;
    }
}

int ugdr_close_device(struct ugdr_context* ctx){
    if (ctx == nullptr) return -1;
    try {
        // 1. build request
        struct ugdr::ipc::ugdr_request req;
        struct ugdr::ipc::ugdr_response rsp;

        req = {
            .header = {
                .magic = ugdr::ipc::UGDR_PROTO_MAGIC,
                .cmd = ugdr::ipc::Cmd::UGDR_CMD_CLOSE_DEVICE,
                .status = 0,
            },
        };

        // 2. send req and handle rsp
        ugdr::lib::IpcClient::sendReqAndHandleRsp(ctx->sock.get_fd(), req, rsp);

        // 3. posthandle: close socket and free ctx
        ctx->sock.close_fd();
        delete ctx;
        return 0;
    } catch(const std::exception& e){
        UGDR_LOG_ERROR("[Client]: Failed to close device %s: %s", ctx->dev_name, e.what());
        return -1;
    }
}
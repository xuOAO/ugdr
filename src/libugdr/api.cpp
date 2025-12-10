#include "../../include/ugdr.h"
#include "../common/logger.h"
#include "ugdr_internal.h"
#include "ipc_client.h"

struct ugdr_context* ugdr_open_device(const char* dev_name){
    if (dev_name == nullptr) return nullptr;
    try{
        struct ugdr_context* ctx = new ugdr_context();
        if (ctx){
            *ctx = ugdr::lib::IpcClient::connect_and_handshake(dev_name);
            std::strncpy(ctx->dev_name, dev_name, sizeof(ctx->dev_name));
        } else{
            throw std::runtime_error("No memory to create context");
        }
        return ctx;
    }catch(const std::exception& e){
        UGDR_LOG_ERROR("[Client]: Failed to open device %s: %s", dev_name, e.what());
        return nullptr;
    }
}

int ugdr_close_device(struct ugdr_context* ctx){
    if (ctx == nullptr) return -1;
    try{
        ugdr::lib::IpcClient::close(ctx);
        return 0;
    }catch(const std::exception& e){
        UGDR_LOG_ERROR("[Client]: Failed to close device %s: %s", ctx->dev_name, e.what());
        return -1;
    }
}
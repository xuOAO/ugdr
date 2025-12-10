#pragma once
#include <cstdint>
#include "../ugdr_types.h"

namespace ugdr{
namespace ipc {
//TODO: 更多的协议内容
constexpr uint32_t UGDR_PROTO_MAGIC = 0x55474552;

constexpr const char* CmdStr[] = {
    "UGDR_CMD_INIT",
    "UGDR_CMD_EXIT",
    "UGDR_CMD_RESP",
    "UGDR_CMD_ERR",
};

enum class Cmd : uint32_t {
    UGDR_CMD_INIT = 0,
    UGDR_CMD_EXIT = 1,
    UGDR_CMD_RESP = 2,
    UGDR_CMD_ERR = 3,
};

struct DeviceName {
    char name[common::UGDR_MAX_DEV_NAME_LEN];
};

struct Header {
    uint32_t magic = UGDR_PROTO_MAGIC;
    Cmd cmd;
    uint32_t payload_len;
};


struct InitReq {
    Header header;
    DeviceName device_name;
};

struct InitRsp {
    Header header;
    uint32_t ctx_idx;
};

struct ExitReq {
    Header header;
};

struct ExitRsp {
    Header header;
};

}
}
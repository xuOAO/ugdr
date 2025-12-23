#pragma once
#include <bits/stdint-uintn.h>
#include <cstdint>
#include <cstddef>
#include "../ugdr_types.h"

namespace ugdr{
namespace ipc {
//TODO: 更多的协议内容
constexpr uint32_t UGDR_PROTO_MAGIC = 0x55474452;

enum class Cmd : uint32_t {
    UGDR_CMD_OPEN_DEVICE = 0,
    UGDR_CMD_CLOSE_DEVICE,
    UGDR_CMD_ALLOC_PD,
    UGDR_CMD_DEALLOC_PD,
    UGDR_CMD_CREATE_CQ,
    UGDR_CMD_DESTROY_CQ,
    UGDR_CMD_CREATE_QP,
    UGDR_CMD_MODIFY_QP,
    UGDR_CMD_DESTROY_QP,
    UGDR_CMD_REG_MR,
    UGDR_CMD_DEREG_MR,
    //experimental cmds
    UGDR_CMD_EXPERIMENTAL = 1000,
};

constexpr const char* cmdToString(Cmd cmd) {
    switch (cmd) {
        case Cmd::UGDR_CMD_OPEN_DEVICE: return "UGDR_CMD_OPEN_DEVICE";
        case Cmd::UGDR_CMD_CLOSE_DEVICE: return "UGDR_CMD_CLOSE_DEVICE";
        case Cmd::UGDR_CMD_ALLOC_PD: return "UGDR_CMD_ALLOC_PD";
        case Cmd::UGDR_CMD_DEALLOC_PD: return "UGDR_CMD_DEALLOC_PD";
        case Cmd::UGDR_CMD_CREATE_CQ: return "UGDR_CMD_CREATE_CQ";
        case Cmd::UGDR_CMD_DESTROY_CQ: return "UGDR_CMD_DESTROY_CQ";
        case Cmd::UGDR_CMD_CREATE_QP: return "UGDR_CMD_CREATE_QP";
        case Cmd::UGDR_CMD_MODIFY_QP: return "UGDR_CMD_MODIFY_QP";
        case Cmd::UGDR_CMD_DESTROY_QP: return "UGDR_CMD_DESTROY_QP";
        case Cmd::UGDR_CMD_REG_MR: return "UGDR_CMD_REG_MR";
        case Cmd::UGDR_CMD_DEREG_MR: return "UGDR_CMD_DEREG_MR";
        //experimental cmds
        case Cmd::UGDR_CMD_EXPERIMENTAL: return "UGDR_CMD_EXPERIMENTAL";
        default: return "UNKNOWN_CMD";
    }
}

// app的sock对象与唯一context对应
struct ugdr_open_dev_req {
    char dev_name[common::UGDR_MAX_DEV_NAME_LEN];
};

struct ugdr_open_dev_rsp {
};

struct ugdr_alloc_pd_req {
};

struct ugdr_alloc_pd_rsp {
    uint32_t pd_handle;
};

struct ugdr_create_cq_req {
    int cqe;
};

struct ugdr_create_cq_rsp {
    char shring_name[common::UGDR_MAX_SHRING_NAME_LEN];
    size_t shring_size;
    uint32_t cq_handle;
};

struct ugdr_create_qp_req {
    uint32_t pd_handle;
    struct {
        uint32_t send_cq_handle;
        uint32_t recv_cq_handle;
        struct {
            uint32_t max_send_wr;
            uint32_t max_recv_wr;
            uint32_t max_sge; // 目前默认 max_sge = 1
        } cap;
        int qp_type; // 目前默认 qp_type = rc
        int sq_sig_all;
    } qp_attr;
};

//TODO：应该返回很多qp属性
struct ugdr_create_qp_rsp {
    uint32_t qp_handle;
    char sq_name[common::UGDR_MAX_SHRING_NAME_LEN];
    char rq_name[common::UGDR_MAX_SHRING_NAME_LEN];
    size_t sq_size;
    size_t rq_size;
};

struct ugdr_destroy_qp_req {
    uint32_t pd_handle;
    uint32_t qp_handle;
};

struct ugdr_destroy_qp_rsp {
};

struct ugdr_modify_qp_req {
    uint32_t qp_handle;
    struct {
        uint32_t qp_state;
        uint32_t cur_qp_state;
        uint32_t dest_qp_num;
    } qp_attr;
    uint32_t attr_mask;
};

struct ugdr_modify_qp_rsp {
};

//TODO: 应当从cudaIPC的角度思考
struct ugdr_reg_mr_req {
    uint32_t pd_handle;
    uint64_t addr;
    uint64_t length;
    int access;
};

struct ugdr_reg_mr_rsp {
    union{
        uint32_t mr_handle;
        uint32_t lkey;
    };
    uint32_t rkey;
};

//TODO: 后续拆散，目前destroy_qp已经拆离
struct ugdr_destroy_rsrc_req {
    union {
        uint32_t pd_handle;
        uint32_t cq_handle;
        uint32_t qp_handle;
        uint32_t mr_handle;
    } handle;
};

struct ugdr_destroy_rsrc_rsp {
};

struct ugdr_experimental_req {
    // experimental fields
    int type; // 0 = cq, 1 = qp_rq, 2 = qp_sq
    union{
        struct {
            uint32_t cq_handle;
        } cq;
        struct {
            uint32_t pd_handle;
            uint32_t qp_handle;
        } qp;
    };
};

struct ugdr_experimental_rsp {
    // experimental fields
    int data;
};

struct ugdr_cmd_header {
    uint32_t magic;
    Cmd cmd;
    int status; // 0表示成功，非0表示失败
};

struct ugdr_request {
    struct ugdr_cmd_header header;
    union{
        struct ugdr_open_dev_req open_dev_req;
        struct ugdr_alloc_pd_req alloc_pd_req;
        struct ugdr_create_cq_req create_cq_req;
        struct ugdr_create_qp_req create_qp_req;
        struct ugdr_modify_qp_req modify_qp_req;
        //TODO: 拆分
        struct ugdr_destroy_rsrc_req destroy_rsrc_req;
        struct ugdr_destroy_qp_req destroy_qp_req;
        // experimental cmds
        struct ugdr_experimental_req experimental_req;
    };
};

struct ugdr_response {
    struct ugdr_cmd_header header;
    union{
        struct ugdr_open_dev_rsp open_dev_rsp;
        struct ugdr_alloc_pd_rsp alloc_pd_rsp;
        struct ugdr_create_cq_rsp create_cq_rsp;
        struct ugdr_create_qp_rsp create_qp_rsp;
        struct ugdr_modify_qp_rsp modify_qp_rsp;
        //TODO: 拆分
        struct ugdr_destroy_rsrc_rsp destroy_rsrc_rsp;
        struct ugdr_destroy_qp_rsp destroy_qp_rsp;
        // experimental cmds
        struct ugdr_experimental_rsp experimental_rsp;
    };
};

}
}
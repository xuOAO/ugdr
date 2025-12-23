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
            struct ugdr::ipc::ugdr_response rsp;

            std::strncpy(ctx->dev_name, dev_name, sizeof(ctx->dev_name));
            ctx->sock = ugdr::ipc::connect_to_server(ugdr::ipc::UDS_PATH_DEFAULT);

            // 2. build request
            struct ugdr::ipc::ugdr_request req = {
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
        struct ugdr::ipc::ugdr_response rsp;
        struct ugdr::ipc::ugdr_request req = {
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

struct ugdr_pd* ugdr_alloc_pd(struct ugdr_context* ctx){
    if (ctx == nullptr) return nullptr;
    try {
        // 1. build request
        struct ugdr::ipc::ugdr_response rsp;
        struct ugdr::ipc::ugdr_request req = {
            .header = {
                .magic = ugdr::ipc::UGDR_PROTO_MAGIC,
                .cmd = ugdr::ipc::Cmd::UGDR_CMD_ALLOC_PD,
                .status = 0,
            },
        };

        // 2. send req and handle rsp
        ugdr::lib::IpcClient::sendReqAndHandleRsp(ctx->sock.get_fd(), req, rsp);

        // 3. posthandle: create ugdr_pd and return
        struct ugdr_pd* pd = new struct ugdr_pd();
        if(pd) {
            pd->sock_ptr = &ctx->sock;
            pd->pd_handle = rsp.alloc_pd_rsp.pd_handle;
        } else {
            throw std::runtime_error("No memory to create pd");
        }
        return pd;
    } catch(const std::exception& e){
        UGDR_LOG_ERROR("[Client]: Failed to alloc pd on device %s: %s", ctx->dev_name, e.what());
        return nullptr;
    }
}

int ugdr_dealloc_pd(struct ugdr_context* ctx, struct ugdr_pd* pd){
    if (ctx == nullptr || pd == nullptr) return -1;
    try {
        // 1. build request
        struct ugdr::ipc::ugdr_response rsp;

        struct ugdr::ipc::ugdr_request req = {
            .header = {
                .magic = ugdr::ipc::UGDR_PROTO_MAGIC,
                .cmd = ugdr::ipc::Cmd::UGDR_CMD_DEALLOC_PD,
                .status = 0,
            },
            .destroy_rsrc_req= {
                .handle = {
                    .pd_handle = pd->pd_handle,
                },
            },
        };

        // 2. send req and handle rsp
        ugdr::lib::IpcClient::sendReqAndHandleRsp(ctx->sock.get_fd(), req, rsp);

        // 3. posthandle: free pd
        delete pd;
        return 0;
    } catch(const std::exception& e){
        UGDR_LOG_ERROR("[Client]: Failed to dealloc pd on device %s: %s", ctx->dev_name, e.what());
        return -1;
    }
}

struct ugdr_cq* ugdr_create_cq(struct ugdr_context* ctx, int wqe){
    if (ctx == nullptr) return nullptr;
    if (wqe <= 0) return nullptr;
    try {
        // 1. build request
        struct ugdr::ipc::ugdr_response rsp;
        struct ugdr::ipc::ugdr_request req = {
            .header = {
                .magic = ugdr::ipc::UGDR_PROTO_MAGIC,
                .cmd = ugdr::ipc::Cmd::UGDR_CMD_CREATE_CQ,
                .status = 0,
            },
            .create_cq_req = {
                .cqe = wqe,
            },
        };

        // 2. send req and handle rsp
        std::vector<int> shm_fd({-1});
        ugdr::lib::IpcClient::sendReqAndHandleRspWithFds(ctx->sock.get_fd(), req, rsp, shm_fd);

        // 3. posthandle: create ugdr_cq and return
        // TODO: 考虑重构初始化，当前可读性太差
        ugdr::ipc::Shmem shmem = ugdr::ipc::Shmem(std::string(rsp.create_cq_rsp.shring_name), rsp.create_cq_rsp.shring_size, shm_fd[0]);
        struct ugdr_cq* cq = new struct ugdr_cq({&ctx->sock, std::move(shmem), rsp.create_cq_rsp.cq_handle});
        return cq;
    } catch(const std::exception& e){
        UGDR_LOG_ERROR("[Client]: Failed to create cq on device %s: %s", ctx->dev_name, e.what());
        return nullptr;
    }
}

int ugdr_destroy_cq(struct ugdr_cq *cq){
    if(cq == nullptr) return -1;
    try {
        // 1. build request
        struct ugdr::ipc::ugdr_response rsp;
        struct ugdr::ipc::ugdr_request req = {
            .header = {
                .magic = ugdr::ipc::UGDR_PROTO_MAGIC,
                .cmd = ugdr::ipc::Cmd::UGDR_CMD_DESTROY_CQ,
                .status = 0,
            },
            .destroy_rsrc_req = {
                .handle = {
                    .cq_handle = cq->cq_handle
                }
            }
        };

        // 2. send req and handle rsp
        ugdr::lib::IpcClient::sendReqAndHandleRsp(cq->sock_ptr->get_fd(), req, rsp);

        // 3. posthandle: free cq
        delete cq;
        return 0;
    } catch(const std::exception& e){
        UGDR_LOG_ERROR("[Client]: Failed to destroy cq: %s", e.what());
        return -1;
    }
}

struct ugdr_qp* ugdr_create_qp(struct ugdr_pd* pd, struct ugdr_qp_init_attr* init_attr){
    if (pd == nullptr || init_attr == nullptr) return nullptr;
    try { 
        // 1. build request
        struct ugdr::ipc::ugdr_response rsp;
        struct ugdr::ipc::ugdr_request req = {
            .header = {
                .magic = ugdr::ipc::UGDR_PROTO_MAGIC,
                .cmd = ugdr::ipc::Cmd::UGDR_CMD_CREATE_QP,
                .status = 0,
            },
            .create_qp_req = {
                .pd_handle = pd->pd_handle,
                .qp_attr = {
                    .send_cq_handle = init_attr->send_cq->cq_handle,
                    .recv_cq_handle = init_attr->recv_cq->cq_handle,
                    .cap = {
                        .max_send_wr = init_attr->cap.max_send_wr,
                        .max_recv_wr = init_attr->cap.max_recv_wr,
                        .max_sge= init_attr->cap.max_sge,
                    },
                    .qp_type = init_attr->qp_type,
                    .sq_sig_all = init_attr->sq_sig_all,
                }
            }
        };
        // 2. send req and handle rsp
        std::vector<int> shm_fd({-1, -1}); //{rq_fd, sq_fd}
        ugdr::lib::IpcClient::sendReqAndHandleRspWithFds(pd->sock_ptr->get_fd(), req, rsp, shm_fd);

        // 3. posthandle: create ugdr_qp and return
        // TODO: 考虑重构初始化，当前可读性太差
        ugdr::ipc::Shmem rq = ugdr::ipc::Shmem(std::string(rsp.create_qp_rsp.rq_name), rsp.create_qp_rsp.rq_size, shm_fd[0]);
        ugdr::ipc::Shmem sq = ugdr::ipc::Shmem(std::string(rsp.create_qp_rsp.sq_name), rsp.create_qp_rsp.sq_size, shm_fd[1]);
        struct ugdr_qp* qp = new struct ugdr_qp({pd->sock_ptr, pd->pd_handle, rsp.create_qp_rsp.qp_handle, std::move(rq), std::move(sq), init_attr->qp_context, init_attr->cap.max_sge, init_attr->qp_type, init_attr->sq_sig_all});

        return qp;
    } catch(const std::exception& e){
        UGDR_LOG_ERROR("[Client]: Failed to create qp: %s", e.what());
        return nullptr;
    }
    return nullptr;
}

int ugdr_destroy_qp(struct ugdr_qp* qp){
    if(qp == nullptr) return -1;
    try {
        // 1. build request
        struct ugdr::ipc::ugdr_response rsp;
        struct ugdr::ipc::ugdr_request req = {
            .header = {
                .magic = ugdr::ipc::UGDR_PROTO_MAGIC,
                .cmd = ugdr::ipc::Cmd::UGDR_CMD_DESTROY_QP,
                .status = 0,
            },
            .destroy_qp_req = {
                .pd_handle = qp->pd_handle,
                .qp_handle = qp->qp_handle,
            }
        };
        // 2. send req and handle rsp
        ugdr::lib::IpcClient::sendReqAndHandleRsp(qp->sock_ptr->get_fd(), req, rsp);

        // 3. posthandle: free qp
        delete qp;
        return 0;
    } catch(const std::exception& e) {
        UGDR_LOG_ERROR("[Client]: Failed to destroy qp: %s", e.what());
        return -1;
    }
}

//experimental api
int ugdr_experimental_write_cq(struct ugdr_cq* cq, int word){
    if (cq == nullptr) return -1;
    try {
        cq->shmem.write(&word, sizeof(word));
        return 0;
    } catch(const std::exception& e){
        UGDR_LOG_ERROR("[Client]: Failed to write cq: %s", e.what());
        return -1;
    }
}

int ugdr_experimental_read_cq(struct ugdr_cq* cq){
    if (cq == nullptr) return -1;
    try {
        // 1. build request
        struct ugdr::ipc::ugdr_response rsp;
        struct ugdr::ipc::ugdr_request req = {
            .header = {
                .magic = ugdr::ipc::UGDR_PROTO_MAGIC,
                .cmd = ugdr::ipc::Cmd::UGDR_CMD_EXPERIMENTAL,
                .status = 0,
            },
            .experimental_req = {
                .type = 0,
                .cq = {
                    .cq_handle = cq->cq_handle,
                }
            },
        };

        // 2. send req and handle rsp
        ugdr::lib::IpcClient::sendReqAndHandleRsp(cq->sock_ptr->get_fd(), req, rsp);

        // 3. return result
        return rsp.experimental_rsp.data;
    } catch(const std::exception& e){
        UGDR_LOG_ERROR("[Client]: Failed to read cq: %s", e.what());
        return -1;
    }
}

int ugdr_experimental_write_qp_sq(struct ugdr_qp* qp, int word){
    if (qp == nullptr) return -1;
    try {
        qp->sq.write(&word, sizeof(word));
        return 0;
    } catch(const std::exception& e){
        UGDR_LOG_ERROR("[Client]: Failed to write cq: %s", e.what());
        return -1;
    }
}

int ugdr_experimental_write_qp_rq(struct ugdr_qp* qp, int word){
    if (qp == nullptr) return -1;
    try {
        qp->rq.write(&word, sizeof(word));
        return 0;
    } catch(const std::exception& e){
        UGDR_LOG_ERROR("[Client]: Failed to write cq: %s", e.what());
        return -1;
    }
}

int ugdr_experimental_read_qp_sq(struct ugdr_qp* qp){
    if (qp == nullptr) return -1;
    try {
        // 1. build request
        struct ugdr::ipc::ugdr_response rsp;
        struct ugdr::ipc::ugdr_request req = {
            .header = {
                .magic = ugdr::ipc::UGDR_PROTO_MAGIC,
                .cmd = ugdr::ipc::Cmd::UGDR_CMD_EXPERIMENTAL,
                .status = 0,
            },
            .experimental_req = {
                .type = 1,
                .qp = {
                    .pd_handle = qp->pd_handle,
                    .qp_handle = qp->qp_handle,
                }
            },
        };

        // 2. send req and handle rsp
        ugdr::lib::IpcClient::sendReqAndHandleRsp(qp->sock_ptr->get_fd(), req, rsp);

        // 3. return result
        return rsp.experimental_rsp.data;
    } catch(const std::exception& e){
        UGDR_LOG_ERROR("[Client]: Failed to read cq: %s", e.what());
        return -1;
    }
}

int ugdr_experimental_read_qp_rq(struct ugdr_qp* qp){
    if (qp == nullptr) return -1;
    try {
        // 1. build request
        struct ugdr::ipc::ugdr_response rsp;
        struct ugdr::ipc::ugdr_request req = {
            .header = {
                .magic = ugdr::ipc::UGDR_PROTO_MAGIC,
                .cmd = ugdr::ipc::Cmd::UGDR_CMD_EXPERIMENTAL,
                .status = 0,
            },
            .experimental_req = {
                .type = 2,
                .qp = {
                    .pd_handle = qp->pd_handle,
                    .qp_handle = qp->qp_handle,
                }
            },
        };

        // 2. send req and handle rsp
        ugdr::lib::IpcClient::sendReqAndHandleRsp(qp->sock_ptr->get_fd(), req, rsp);

        // 3. return result
        return rsp.experimental_rsp.data;
    } catch(const std::exception& e){
        UGDR_LOG_ERROR("[Client]: Failed to read cq: %s", e.what());
        return -1;
    }
}
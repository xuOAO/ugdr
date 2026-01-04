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

struct ugdr_mr* ugdr_reg_mr(struct ugdr_pd* pd, void* addr, size_t length, int access){
    if (pd == nullptr || addr == nullptr || length == 0) return nullptr;
    try {
        // 1. build request
        struct ugdr::ipc::ugdr_response rsp;
        struct ugdr::ipc::ugdr_request req = {
            .header = {
                .magic = ugdr::ipc::UGDR_PROTO_MAGIC,
                .cmd = ugdr::ipc::Cmd::UGDR_CMD_REG_MR,
                .status = 0,
            },
            .reg_mr_req = {
                .pd_handle = pd->pd_handle,
                .addr = (uint64_t)addr,
                .length = length,
                .access = access,
            }
        };

        // 2. send req and handle rsp
        ugdr::lib::IpcClient::sendReqAndHandleRsp(pd->sock_ptr->get_fd(), req, rsp);

        // 3. posthandle: create ugdr_mr and return
        struct ugdr_mr* mr = new struct ugdr_mr();
        if(mr) {
            mr->pd = pd;
            mr->lkey = rsp.reg_mr_rsp.lkey;
            mr->rkey = rsp.reg_mr_rsp.lkey;
            mr->addr = addr;
            mr->length = length;
        } else {
            throw std::runtime_error("No memory to create mr");
        }
        return mr;
    } catch(const std::exception& e){
        UGDR_LOG_ERROR("[Client]: Failed to reg mr: %s", e.what());
        return nullptr;
    }
}

int ugdr_dereg_mr(struct ugdr_mr* mr){
    if(mr == nullptr) return -1;
    try {
        // 1. build request
        struct ugdr::ipc::ugdr_response rsp;
        struct ugdr::ipc::ugdr_request req = {
            .header = {
                .magic = ugdr::ipc::UGDR_PROTO_MAGIC,
                .cmd = ugdr::ipc::Cmd::UGDR_CMD_DEREG_MR,
                .status = 0,
            },
            .dereg_mr_req = {
                .pd_handle = mr->pd->pd_handle,
                .lkey = mr->lkey,
            }
        };

        // 2. send req and handle rsp
        ugdr::lib::IpcClient::sendReqAndHandleRsp(mr->pd->sock_ptr->get_fd(), req, rsp);

        // 3. posthandle: free mr
        delete mr;
        return 0;
    } catch(const std::exception& e) {
        UGDR_LOG_ERROR("[Client]: Failed to dereg mr: %s", e.what());
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
        std::string cq_name = std::string(rsp.create_cq_rsp.shring_name);
        ugdr::ipc::SpscShmRing<ugdr::common::Cqe> shring(cq_name, wqe, shm_fd[0]);
        struct ugdr_cq* cq = new struct ugdr_cq({&ctx->sock, std::move(shring), rsp.create_cq_rsp.cq_handle});
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
        // ugdr::ipc::Shmem rq = ugdr::ipc::Shmem(std::string(rsp.create_qp_rsp.rq_name), rsp.create_qp_rsp.rq_size, shm_fd[0]);
        // ugdr::ipc::Shmem sq = ugdr::ipc::Shmem(std::string(rsp.create_qp_rsp.sq_name), rsp.create_qp_rsp.sq_size, shm_fd[1]);
        std::string rq_name = std::string(rsp.create_qp_rsp.rq_name);
        std::string sq_name = std::string(rsp.create_qp_rsp.sq_name);
        ugdr::ipc::SpscShmRing<ugdr::common::Wqe> rq(rq_name, init_attr->cap.max_recv_wr, shm_fd[0]);
        ugdr::ipc::SpscShmRing<ugdr::common::Wqe> sq(sq_name, init_attr->cap.max_send_wr, shm_fd[1]);
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

int ugdr_post_send(struct ugdr_qp *qp, struct ugdr_send_wr *wr, struct ugdr_send_wr **bad_wr) {
    if (!qp || !wr) return -1;

    constexpr int BATCH_SIZE = 32;
    ugdr::common::Wqe wqes[BATCH_SIZE];
    struct ugdr_send_wr* wr_ptrs[BATCH_SIZE];
    int count = 0;

    struct ugdr_send_wr *curr = wr;
    while (curr) {
        ugdr::common::Wqe& wqe = wqes[count];
        wr_ptrs[count] = curr;

        wqe = {};
        wqe.wr_id = curr->wr_id;
        
        switch (curr->opcode) {
            case UGDR_WR_RDMA_WRITE: wqe.opcode = ugdr::common::WrOpcode::RDMA_WRITE; break;
            case UGDR_WR_RDMA_WRITE_WITH_IMM: wqe.opcode = ugdr::common::WrOpcode::RDMA_WRITE_WITH_IMM; break;
            case UGDR_WR_SEND: wqe.opcode = ugdr::common::WrOpcode::SEND; break;
            case UGDR_WR_SEND_WITH_IMM: wqe.opcode = ugdr::common::WrOpcode::SEND_WITH_IMM; break;
            case UGDR_WR_RDMA_READ: wqe.opcode = ugdr::common::WrOpcode::RDMA_READ; break;
            case UGDR_WR_ATOMIC_CMP_AND_SWP: wqe.opcode = ugdr::common::WrOpcode::ATOMIC_CMP_AND_SWP; break;
            case UGDR_WR_ATOMIC_FETCH_AND_ADD: wqe.opcode = ugdr::common::WrOpcode::ATOMIC_FETCH_AND_ADD; break;
            default: 
                if (bad_wr) *bad_wr = curr;
                return -1;
        }

        wqe.flags = curr->send_flags;
        wqe.qp_num = qp->qp_handle;

        if (curr->num_sge > 0 && curr->sg_list) {
            //TODO: 目前只支持1 sge
            wqe.sge.addr = curr->sg_list[0].addr;
            wqe.sge.length = curr->sg_list[0].length;
            wqe.sge.lkey = curr->sg_list[0].lkey;
        }

        if (curr->opcode == UGDR_WR_RDMA_WRITE || curr->opcode == UGDR_WR_RDMA_WRITE_WITH_IMM || curr->opcode == UGDR_WR_RDMA_READ) {
            wqe.rdma.remote_addr = curr->wr.rdma.remote_addr;
            wqe.rdma.rkey = curr->wr.rdma.rkey;
        }
        
        if (curr->opcode == UGDR_WR_SEND_WITH_IMM || curr->opcode == UGDR_WR_RDMA_WRITE_WITH_IMM) {
             wqe.send.imm_data = curr->imm_data;
        }

        count++;
        if (count == BATCH_SIZE) {
            int pushed = qp->sq.push_batch(wqes, count);
            if (pushed < count) {
                if (bad_wr) *bad_wr = wr_ptrs[pushed]; 
                return -1;
            }
            count = 0;
        }

        curr = curr->next;
    }

    if (count > 0) {
        int pushed = qp->sq.push_batch(wqes, count);
        if (pushed < count) {
            if (bad_wr) *bad_wr = wr_ptrs[pushed]; 
            return -1;
        }
    }
    return 0;
}

int ugdr_post_recv(struct ugdr_qp *qp, struct ugdr_recv_wr *wr, struct ugdr_recv_wr **bad_wr) {
    if (!qp || !wr) return -1;

    constexpr int BATCH_SIZE = 32;
    ugdr::common::Wqe wqes[BATCH_SIZE];
    struct ugdr_recv_wr* wr_ptrs[BATCH_SIZE];
    int count = 0;

    struct ugdr_recv_wr *curr = wr;
    while (curr) {
        ugdr::common::Wqe& wqe = wqes[count];
        wr_ptrs[count] = curr;

        wqe = {};
        wqe.wr_id = curr->wr_id;
        wqe.opcode = ugdr::common::WrOpcode::RECV;
        wqe.qp_num = qp->qp_handle;

        if (curr->num_sge > 0 && curr->sg_list) {
            //TODO: 目前只支持1 sge
            wqe.sge.addr = curr->sg_list[0].addr;
            wqe.sge.length = curr->sg_list[0].length;
            wqe.sge.lkey = curr->sg_list[0].lkey;
        }

        count++;
        if (count == BATCH_SIZE) {
            int pushed = qp->rq.push_batch(wqes, count);
            if (pushed < count) {
                if (bad_wr) *bad_wr = wr_ptrs[pushed];
                return -1;
            }
            count = 0;
        }
        curr = curr->next;
    }

    if (count > 0) {
        int pushed = qp->rq.push_batch(wqes, count);
        if (pushed < count) {
            if (bad_wr) *bad_wr = wr_ptrs[pushed];
            return -1;
        }
    }
    return 0;
}

static void map_cqe_to_wc(const ugdr::common::Cqe& cqe, struct ugdr_wc& wc) {
    wc.vendor_err = 0;
    wc.pkey_index = 0;
    wc.slid = 0;
    wc.sl = 0;
    wc.dlid_path_bits = 0;
    wc.wr_id = cqe.wr_id;
    wc.byte_len = cqe.byte_len;
    wc.qp_num = cqe.qp_num;
    wc.src_qp = cqe.src_qp;
    wc.wc_flags = cqe.wc_flags;
    wc.imm_data = cqe.imm_data;

    // Map Status
    switch (static_cast<ugdr::common::WcStatus>(cqe.status)) {
        case ugdr::common::WcStatus::SUCCESS: wc.status = UGDR_WC_SUCCESS; break;
        case ugdr::common::WcStatus::LOC_LEN_ERR: wc.status = UGDR_WC_LOC_LEN_ERR; break;
        case ugdr::common::WcStatus::LOC_QP_OP_ERR: wc.status = UGDR_WC_LOC_QP_OP_ERR; break;
        case ugdr::common::WcStatus::LOC_EEC_OP_ERR: wc.status = UGDR_WC_LOC_EEC_OP_ERR; break;
        case ugdr::common::WcStatus::LOC_PROT_ERR: wc.status = UGDR_WC_LOC_PROT_ERR; break;
        case ugdr::common::WcStatus::WR_FLUSH_ERR: wc.status = UGDR_WC_WR_FLUSH_ERR; break;
        case ugdr::common::WcStatus::MW_BIND_ERR: wc.status = UGDR_WC_MW_BIND_ERR; break;
        case ugdr::common::WcStatus::BAD_RESP_ERR: wc.status = UGDR_WC_BAD_RESP_ERR; break;
        case ugdr::common::WcStatus::LOC_ACCESS_ERR: wc.status = UGDR_WC_LOC_ACCESS_ERR; break;
        case ugdr::common::WcStatus::REM_INV_REQ_ERR: wc.status = UGDR_WC_REM_INV_REQ_ERR; break;
        case ugdr::common::WcStatus::REM_ACCESS_ERR: wc.status = UGDR_WC_REM_ACCESS_ERR; break;
        case ugdr::common::WcStatus::REM_OP_ERR: wc.status = UGDR_WC_REM_OP_ERR; break;
        case ugdr::common::WcStatus::RETRY_EXC_ERR: wc.status = UGDR_WC_RETRY_EXC_ERR; break;
        case ugdr::common::WcStatus::RNR_RETRY_EXC_ERR: wc.status = UGDR_WC_RNR_RETRY_EXC_ERR; break;
        case ugdr::common::WcStatus::LOC_RDD_VIOL_ERR: wc.status = UGDR_WC_LOC_RDD_VIOL_ERR; break;
        case ugdr::common::WcStatus::REM_INV_RD_REQ_ERR: wc.status = UGDR_WC_REM_INV_RD_REQ_ERR; break;
        case ugdr::common::WcStatus::REM_ABORT_ERR: wc.status = UGDR_WC_REM_ABORT_ERR; break;
        case ugdr::common::WcStatus::INV_EECN_ERR: wc.status = UGDR_WC_INV_EECN_ERR; break;
        case ugdr::common::WcStatus::INV_EEC_STATE_ERR: wc.status = UGDR_WC_INV_EEC_STATE_ERR; break;
        case ugdr::common::WcStatus::FATAL_ERR: wc.status = UGDR_WC_FATAL_ERR; break;
        case ugdr::common::WcStatus::RESP_TIMEOUT_ERR: wc.status = UGDR_WC_RESP_TIMEOUT_ERR; break;
        case ugdr::common::WcStatus::GENERAL_ERR: wc.status = UGDR_WC_GENERAL_ERR; break;
        default: wc.status = UGDR_WC_GENERAL_ERR; break;
    }

    // Map Opcode
    switch (static_cast<ugdr::common::WrOpcode>(cqe.opcode)) {
        // SQ Completions (Sender side) - NO IMM flag
        case ugdr::common::WrOpcode::SEND: 
            wc.opcode = UGDR_WC_SEND; 
            break;
        case ugdr::common::WrOpcode::SEND_WITH_IMM: 
            wc.opcode = UGDR_WC_SEND; 
            // Explicitly NOT setting UGDR_WC_WITH_IMM for sender
            break;
        case ugdr::common::WrOpcode::RDMA_WRITE: 
            wc.opcode = UGDR_WC_RDMA_WRITE; 
            break;
        case ugdr::common::WrOpcode::RDMA_WRITE_WITH_IMM: 
            wc.opcode = UGDR_WC_RDMA_WRITE; 
            // Explicitly NOT setting UGDR_WC_WITH_IMM for sender
            break;
        case ugdr::common::WrOpcode::RDMA_READ: 
            wc.opcode = UGDR_WC_RDMA_READ; 
            break;
        case ugdr::common::WrOpcode::ATOMIC_CMP_AND_SWP: 
            wc.opcode = UGDR_WC_COMP_SWAP; 
            break;
        case ugdr::common::WrOpcode::ATOMIC_FETCH_AND_ADD: 
            wc.opcode = UGDR_WC_FETCH_ADD; 
            break;
        
        // RQ Completions (Receiver side)
        case ugdr::common::WrOpcode::RECV: 
            wc.opcode = UGDR_WC_RECV; 
            break;
        case ugdr::common::WrOpcode::RECV_RDMA_WITH_IMM: 
            wc.opcode = UGDR_WC_RECV_RDMA_WITH_IMM; 
            wc.wc_flags |= UGDR_WC_WITH_IMM; 
            break;
            
        default: 
            wc.opcode = UGDR_WC_SEND; 
            break;
    }
}

int ugdr_poll_cq(struct ugdr_cq *cq, int num_entries, struct ugdr_wc *wc) {
    if (!cq || !wc || num_entries <= 0) return -1;

    constexpr int BATCH_SIZE = 32;
    ugdr::common::Cqe cqes[BATCH_SIZE];

    int total_polled = 0;
    while (total_polled < num_entries) {
        int to_poll = std::min(num_entries - total_polled, BATCH_SIZE);
        int n = cq->shring.pop_batch(cqes, to_poll);
        
        if (n == 0) break;

        for (int i = 0; i < n; ++i) {
            map_cqe_to_wc(cqes[i], wc[total_polled + i]);
        }
        total_polled += n;
    }
    return total_polled;
}

//experimental api
int ugdr_experimental_write_cq(struct ugdr_cq* cq, int word){
    if (cq == nullptr) return -1;
    try {
        cq->shring.write(&word, sizeof(word));
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

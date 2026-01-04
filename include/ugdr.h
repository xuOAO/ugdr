#ifndef UGDR_H_
#define UGDR_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"{
#endif

struct ugdr_context;
struct ugdr_pd;
struct ugdr_cq;
struct ugdr_qp;
struct ugdr_mr {
    struct ugdr_context *context;
    struct ugdr_pd      *pd;
    void                *addr;
    size_t               length;
    uint32_t             lkey;
    uint32_t             rkey;
};

enum ugdr_access_flags {
    UGDR_ACCESS_LOCAL_WRITE = 1
};

struct ugdr_qp_init_attr {
    void* qp_context; // not used now
    struct ugdr_cq* send_cq;
    struct ugdr_cq* recv_cq;
    struct {
        uint32_t max_send_wr;
        uint32_t max_recv_wr;
        uint32_t max_sge; // now only support sge = 1
    } cap;
    int qp_type; // now only support RC
    int sq_sig_all; // now only support sq_sig_all = 1
};

struct ugdr_context* ugdr_open_device(const char* dev_name);
int ugdr_close_device(struct ugdr_context* ctx);

struct ugdr_pd* ugdr_alloc_pd(struct ugdr_context* ctx);
int ugdr_dealloc_pd(struct ugdr_context* ctx, struct ugdr_pd* pd);

struct ugdr_mr* ugdr_reg_mr(struct ugdr_pd* pd, void* addr, size_t length, int access);
int ugdr_dereg_mr(struct ugdr_mr* mr);

struct ugdr_cq* ugdr_create_cq(struct ugdr_context* ctx, int wqe);
int ugdr_destroy_cq(struct ugdr_cq* cq);

struct ugdr_qp* ugdr_create_qp(struct ugdr_pd* pd, struct ugdr_qp_init_attr* attr);
int ugdr_destroy_qp(struct ugdr_qp* qp);

enum ugdr_wr_opcode {
    UGDR_WR_RDMA_WRITE,
    UGDR_WR_RDMA_WRITE_WITH_IMM,
    UGDR_WR_SEND,
    UGDR_WR_SEND_WITH_IMM,
    UGDR_WR_RDMA_READ,
    UGDR_WR_ATOMIC_CMP_AND_SWP,
    UGDR_WR_ATOMIC_FETCH_AND_ADD
};

enum ugdr_send_flags {
    UGDR_SEND_FENCE = 1 << 0,
    UGDR_SEND_SIGNALED = 1 << 1,
    UGDR_SEND_SOLICITED = 1 << 2,
    UGDR_SEND_INLINE = 1 << 3
};

struct ugdr_sge {
    uint64_t addr;
    uint32_t length;
    uint32_t lkey;
};

struct ugdr_send_wr {
    uint64_t wr_id;
    struct ugdr_send_wr *next;
    struct ugdr_sge *sg_list;
    int num_sge;
    enum ugdr_wr_opcode opcode;
    int send_flags;
    uint32_t imm_data;
    union {
        struct {
            uint64_t remote_addr;
            uint32_t rkey;
        } rdma;
        struct {
            uint64_t remote_addr;
            uint64_t compare_add;
            uint64_t swap;
            uint32_t rkey;
        } atomic;
        struct {
            struct ugdr_ah *ah;
            uint32_t remote_qpn;
            uint32_t remote_qkey;
        } ud;
    } wr;
};

struct ugdr_recv_wr {
    uint64_t wr_id;
    struct ugdr_recv_wr *next;
    struct ugdr_sge *sg_list;
    int num_sge;
};

int ugdr_post_send(struct ugdr_qp *qp, struct ugdr_send_wr *wr, struct ugdr_send_wr **bad_wr);
int ugdr_post_recv(struct ugdr_qp *qp, struct ugdr_recv_wr *wr, struct ugdr_recv_wr **bad_wr);

enum ugdr_wc_status {
    UGDR_WC_SUCCESS = 0,
    UGDR_WC_LOC_LEN_ERR,
    UGDR_WC_LOC_QP_OP_ERR,
    UGDR_WC_LOC_EEC_OP_ERR,
    UGDR_WC_LOC_PROT_ERR,
    UGDR_WC_WR_FLUSH_ERR,
    UGDR_WC_MW_BIND_ERR,
    UGDR_WC_BAD_RESP_ERR,
    UGDR_WC_LOC_ACCESS_ERR,
    UGDR_WC_REM_INV_REQ_ERR,
    UGDR_WC_REM_ACCESS_ERR,
    UGDR_WC_REM_OP_ERR,
    UGDR_WC_RETRY_EXC_ERR,
    UGDR_WC_RNR_RETRY_EXC_ERR,
    UGDR_WC_LOC_RDD_VIOL_ERR,
    UGDR_WC_REM_INV_RD_REQ_ERR,
    UGDR_WC_REM_ABORT_ERR,
    UGDR_WC_INV_EECN_ERR,
    UGDR_WC_INV_EEC_STATE_ERR,
    UGDR_WC_FATAL_ERR,
    UGDR_WC_RESP_TIMEOUT_ERR,
    UGDR_WC_GENERAL_ERR
};

enum ugdr_wc_opcode {
    UGDR_WC_SEND,
    UGDR_WC_RDMA_WRITE,
    UGDR_WC_RDMA_READ,
    UGDR_WC_COMP_SWAP,
    UGDR_WC_FETCH_ADD,
    UGDR_WC_BIND_MW,
    UGDR_WC_LOCAL_INV,
    UGDR_WC_TSO,
    UGDR_WC_RECV,
    UGDR_WC_RECV_RDMA_WITH_IMM
};

enum ugdr_wc_flags {
    UGDR_WC_GRH = 1 << 0,
    UGDR_WC_WITH_IMM = 1 << 1,
    UGDR_WC_IP_CSUM_OK = 1 << 2,
    UGDR_WC_WITH_INV = 1 << 3,
    UGDR_WC_TM_SYNC_REQ = 1 << 4,
    UGDR_WC_TM_MATCH = 1 << 5,
    UGDR_WC_TM_DATA_VALID = 1 << 6
};

struct ugdr_wc {
    uint64_t wr_id;
    enum ugdr_wc_status status;
    enum ugdr_wc_opcode opcode;
    uint32_t vendor_err;
    uint32_t byte_len;
    uint32_t imm_data;
    uint32_t qp_num;
    uint32_t src_qp;
    int wc_flags;
    uint16_t pkey_index;
    uint16_t slid;
    uint8_t sl;
    uint8_t dlid_path_bits;
};

int ugdr_poll_cq(struct ugdr_cq *cq, int num_entries, struct ugdr_wc *wc);

//experimental api
int ugdr_experimental_write_cq(struct ugdr_cq* cq, int word);
int ugdr_experimental_read_cq(struct ugdr_cq* cq);
int ugdr_experimental_write_qp_sq(struct ugdr_qp* qp, int word);
int ugdr_experimental_read_qp_sq(struct ugdr_qp* qp);
int ugdr_experimental_write_qp_rq(struct ugdr_qp* qp, int word);
int ugdr_experimental_read_qp_rq(struct ugdr_qp* qp);

#ifdef __cplusplus
}
#endif

#endif // UGDR_H_

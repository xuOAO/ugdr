#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define UGDR_NOEXCEPT noexcept
extern "C" {
#else
#define UGDR_NOEXCEPT
#endif

typedef struct ugdr_device ugdr_device;
typedef struct ugdr_context ugdr_context;
typedef struct ugdr_pd ugdr_pd;
typedef struct ugdr_mr ugdr_mr;
typedef struct ugdr_cq ugdr_cq;
typedef struct ugdr_comp_channel ugdr_comp_channel;
typedef struct ugdr_qp ugdr_qp;

typedef struct ugdr_qp_init_attr ugdr_qp_init_attr;
typedef struct ugdr_qp_attr ugdr_qp_attr;
typedef struct ugdr_qp_conn_info ugdr_qp_conn_info;
typedef struct ugdr_sge ugdr_sge;
typedef struct ugdr_send_wr ugdr_send_wr;
typedef struct ugdr_recv_wr ugdr_recv_wr;
typedef struct ugdr_wc ugdr_wc;

typedef enum ugdr_qp_type {
    UGDR_QPT_RC = 2,
} ugdr_qp_type;

typedef enum ugdr_qp_state {
    UGDR_QPS_RESET = 0,
    UGDR_QPS_INIT = 1,
    UGDR_QPS_RTR = 2,
    UGDR_QPS_RTS = 3,
    UGDR_QPS_SQD = 4,
    UGDR_QPS_SQE = 5,
    UGDR_QPS_ERR = 6,
    UGDR_QPS_UNKNOWN = 7,
} ugdr_qp_state;

typedef enum ugdr_qp_attr_mask {
    UGDR_QP_STATE = 1U << 0U,
    UGDR_QP_CUR_STATE = 1U << 1U,
    UGDR_QP_ACCESS_FLAGS = 1U << 3U,
    UGDR_QP_TIMEOUT = 1U << 9U,
    UGDR_QP_RETRY_CNT = 1U << 10U,
    UGDR_QP_RNR_RETRY = 1U << 11U,
    UGDR_QP_MIN_RNR_TIMER = 1U << 15U,
} ugdr_qp_attr_mask;

typedef enum ugdr_wr_opcode {
    UGDR_WR_RDMA_WRITE = 0,
    UGDR_WR_RDMA_WRITE_WITH_IMM = 1,
} ugdr_wr_opcode;

typedef enum ugdr_send_flags {
    UGDR_SEND_SIGNALED = 1U << 1U,
} ugdr_send_flags;

typedef enum ugdr_wc_status {
    UGDR_WC_SUCCESS = 0,
    UGDR_WC_LOC_LEN_ERR = 1,
    UGDR_WC_LOC_QP_OP_ERR = 2,
    UGDR_WC_LOC_PROT_ERR = 4,
    UGDR_WC_WR_FLUSH_ERR = 5,
    UGDR_WC_LOC_ACCESS_ERR = 8,
    UGDR_WC_REM_INV_REQ_ERR = 9,
    UGDR_WC_REM_ACCESS_ERR = 10,
    UGDR_WC_REM_OP_ERR = 11,
    UGDR_WC_RETRY_EXC_ERR = 12,
    UGDR_WC_RNR_RETRY_EXC_ERR = 13,
    UGDR_WC_GENERAL_ERR = 21,
} ugdr_wc_status;

typedef enum ugdr_wc_opcode {
    UGDR_WC_RDMA_WRITE = 1,
    UGDR_WC_RECV_RDMA_WITH_IMM = 129,
} ugdr_wc_opcode;

typedef enum ugdr_wc_flags {
    UGDR_WC_WITH_IMM = 1U << 1U,
} ugdr_wc_flags;

typedef enum ugdr_access_flags {
    UGDR_ACCESS_LOCAL_WRITE = 1U << 0U,
    UGDR_ACCESS_REMOTE_WRITE = 1U << 1U,
} ugdr_access_flags;

struct ugdr_mr {
    ugdr_context *context;
    ugdr_pd *pd;
    void *addr;
    size_t length;
    uint32_t handle;
    uint32_t lkey;
    uint32_t rkey;
};

struct ugdr_sge {
    uint64_t addr;
    uint32_t length;
    uint32_t lkey;
};

struct ugdr_send_wr {
    uint64_t wr_id;
    ugdr_send_wr *next;
    ugdr_sge *sg_list;
    int num_sge;
    ugdr_wr_opcode opcode;
    unsigned int send_flags;
    union {
        uint32_t imm_data;
    };
    union {
        struct {
            uint64_t remote_addr;
            uint32_t rkey;
        } rdma;
    } wr;
};

struct ugdr_recv_wr {
    uint64_t wr_id;
    ugdr_recv_wr *next;
    ugdr_sge *sg_list;
    int num_sge;
};

struct ugdr_wc {
    uint64_t wr_id;
    ugdr_wc_status status;
    ugdr_wc_opcode opcode;
    uint32_t vendor_err;
    uint32_t byte_len;
    union {
        uint32_t imm_data;
    };
    uint32_t qp_num;
    uint32_t src_qp;
    unsigned int wc_flags;
    uint16_t pkey_index;
    uint16_t slid;
    uint8_t sl;
    uint8_t dlid_path_bits;
};

struct ugdr_qp_init_attr {
    ugdr_cq *send_cq;
    ugdr_cq *recv_cq;
    uint32_t max_send_wr;
    uint32_t max_recv_wr;
    uint32_t max_send_sge;
    uint32_t max_recv_sge;
    ugdr_qp_type qp_type;
    int sq_sig_all;
};

struct ugdr_qp_attr {
    ugdr_qp_state qp_state;
    ugdr_qp_state cur_qp_state;
    int qp_access_flags;
    uint8_t timeout;
    uint8_t retry_cnt;
    uint8_t rnr_retry;
    uint8_t min_rnr_timer;
};

struct ugdr_qp_conn_info {
    uint32_t qp_num;
    uint64_t endpoint_id;
};

ugdr_device **ugdr_get_device_list(int *num_devices) UGDR_NOEXCEPT;
void ugdr_free_device_list(ugdr_device **list) UGDR_NOEXCEPT;

ugdr_context *ugdr_open_device(ugdr_device *device) UGDR_NOEXCEPT;
int ugdr_close_device(ugdr_context *context) UGDR_NOEXCEPT;

ugdr_pd *ugdr_alloc_pd(ugdr_context *context) UGDR_NOEXCEPT;
int ugdr_dealloc_pd(ugdr_pd *pd) UGDR_NOEXCEPT;

ugdr_mr *ugdr_reg_mr(ugdr_pd *pd, void *address, size_t length, int access) UGDR_NOEXCEPT;
int ugdr_dereg_mr(ugdr_mr *mr) UGDR_NOEXCEPT;

ugdr_cq *ugdr_create_cq(ugdr_context *context, int cqe, void *cq_context,
                        ugdr_comp_channel *channel, int comp_vector) UGDR_NOEXCEPT;
int ugdr_destroy_cq(ugdr_cq *cq) UGDR_NOEXCEPT;
int ugdr_poll_cq(ugdr_cq *cq, int num_entries, ugdr_wc *wc) UGDR_NOEXCEPT;

ugdr_qp *ugdr_create_qp(ugdr_pd *pd, ugdr_qp_init_attr *init_attr) UGDR_NOEXCEPT;
int ugdr_destroy_qp(ugdr_qp *qp) UGDR_NOEXCEPT;
int ugdr_modify_qp(ugdr_qp *qp, ugdr_qp_attr *attr, int attr_mask) UGDR_NOEXCEPT;
int ugdr_query_qp(ugdr_qp *qp, ugdr_qp_attr *attr, int attr_mask,
                  ugdr_qp_init_attr *init_attr) UGDR_NOEXCEPT;

int ugdr_query_qp_conn_info(ugdr_qp *qp, ugdr_qp_conn_info *info) UGDR_NOEXCEPT;
int ugdr_connect_qp(ugdr_qp *qp, const ugdr_qp_conn_info *remote_info, const ugdr_qp_attr *attr,
                    int attr_mask) UGDR_NOEXCEPT;

int ugdr_post_send(ugdr_qp *qp, ugdr_send_wr *wr, ugdr_send_wr **bad_wr) UGDR_NOEXCEPT;
int ugdr_post_recv(ugdr_qp *qp, ugdr_recv_wr *wr, ugdr_recv_wr **bad_wr) UGDR_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#undef UGDR_NOEXCEPT

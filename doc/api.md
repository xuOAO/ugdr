#ifndef _UGDR_API_H_
#define _UGDR_API_H_

#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Macros & Constants
 * ========================================================================= */
#define UGDR_DEVICE_NAME_MAX  64
#define UGDR_MAX_GPUS_PER_DEV 16

/* =========================================================================
 * Enums
 * ========================================================================= */

enum ugdr_access_flags {
    UGDR_ACCESS_LOCAL_WRITE = 1
};

enum ugdr_qp_state {
    UGDR_QPS_RESET,
    UGDR_QPS_INIT,
    UGDR_QPS_RTR,
    UGDR_QPS_RTS,
    UGDR_QPS_ERR
};

enum ugdr_wr_opcode {
    UGDR_WR_SEND,
    UGDR_WR_RDMA_WRITE, // 暂不支持
    UGDR_WR_RDMA_READ   // 暂不支持
};

enum ugdr_wc_status {
    UGDR_WC_SUCCESS = 0,
    UGDR_WC_LOC_LEN_ERR
};

enum ugdr_send_flags {
    UGDR_SEND_SIGNALED = 1,
    UGDR_SEND_INLINE   = (1 << 1)
};

/* =========================================================================
 * Data Structures
 * ========================================================================= */

struct ugdr_device {
    char name[UGDR_DEVICE_NAME_MAX];
    int numa_node;
    int num_gpu;
    int gpus[UGDR_MAX_GPUS_PER_DEV];
};

/* Forward declarations for handles */
struct ugdr_context;
struct ugdr_cq;
struct ugdr_pd;
struct ugdr_qp;
struct ugdr_mr;

struct ibv_qp {
	struct ibv_context     *context;
	void		       *qp_context;
	struct ibv_pd	       *pd;
	struct ibv_cq	       *send_cq;
	struct ibv_cq	       *recv_cq;
	struct ibv_srq	       *srq;
	uint32_t		handle;
	uint32_t		qp_num;
	enum ibv_qp_state       state;
	enum ibv_qp_type	qp_type;

	pthread_mutex_t		mutex;
	pthread_cond_t		cond;
	uint32_t		events_completed;
};

struct ugdr_qp_init_attr {
    void* qp_context;
    struct ugdr_cq* send_cq;
    struct ugdr_cq* recv_cq;
    struct {
        uint32_t max_send_wr;
        uint32_t max_recv_wr;
        uint32_t max_sge; // 暂时保留 max_sge = 1
    } cap;
    int qp_type; // 暂时保留 qp_type = rc
    int sq_sig_all;
};

struct ugdr_qp_attr {
    enum ugdr_qp_state qp_state;
    enum ugdr_qp_state cur_qp_state;
    uint32_t dest_qp_num;
};

/* Work Completion */
struct ugdr_wc {
    uint64_t wr_id;
    enum ugdr_wc_status status;
    enum ugdr_wr_opcode opcode; /* 注意：此处原文为 enum ugdr_wc_opcode，通常 WC opcode 与 WR opcode 类似或复用 */
    uint32_t byte_len;
    uint32_t qp_num;
};

/* Memory Region (Based on comments) */
struct ugdr_mr {
    struct ugdr_context* context;
    struct ugdr_pd* pd;
    void* addr;
    size_t length;
    uint32_t handle;
    uint32_t lkey;
    uint32_t rkey;
};

/* Scatter/Gather Element */
struct ugdr_sge {
    uint64_t addr;
    uint32_t length;
    uint32_t lkey;
};

/* Receive Work Request */
struct ugdr_recv_wr {
    uint64_t wr_id;
    struct ugdr_recv_wr* next;
    struct ugdr_sge* sg_list; // 暂不支持多段 sge
    int num_sge;              // num_sge = 1
};

/* Send Work Request */
struct ugdr_send_wr {
    uint64_t wr_id;
    struct ugdr_sge* sg_list;
    int num_sge;
    enum ugdr_wr_opcode opcode;
    uint32_t send_flags;
    struct {
        uint64_t remote_addr;
        uint32_t rkey;
    } rdma; // 暂不支持
};

/* =========================================================================
 * API Function Prototypes
 * ========================================================================= */

/* Device Management */
struct ugdr_device** ugdr_get_device_list(int *num_devices);
struct ugdr_context* ugdr_open_device(struct ugdr_device* device);
int ugdr_close_device(struct ugdr_device* device);

/* Protection Domain */
struct ugdr_pd* ugdr_alloc_pd(struct ugdr_context* ctx);
int ugdr_dealloc_pd(struct ugdr_pd* pd);

/* Memory Region */
// Note: 'access' param uses enum ugdr_access_flags (or ibv_access_flags compat)
struct ugdr_mr* ugdr_reg_mr(struct ugdr_pd* pd, void* addr, size_t length, int access);
int ugdr_dereg_mr(struct ugdr_mr* mr);

/* Completion Queue */
struct ugdr_cq* ugdr_create_cq(struct ugdr_context* ctx, int cqe);
int ugdr_destroy_cq(struct ugdr_cq* cq);
int ugdr_poll_cq(struct ugdr_cq* cq, int num_entries, struct ugdr_wc *wc);

/* Queue Pair */
struct ugdr_qp* ugdr_create_qp(struct ugdr_pd* pd, struct ugdr_qp_init_attr* init_attr);
int ugdr_modify_qp(struct ugdr_qp* qp, struct ugdr_qp_attr* attr, int attr_mask);
int ugdr_destroy_qp(struct ugdr_qp* qp);

/* Operations */
int ugdr_post_recv(struct ugdr_qp* qp, struct ugdr_recv_wr* wr, struct ugdr_recv_wr** bad_wr);
int ugdr_post_send(struct ugdr_qp* qp, struct ugdr_send_wr* wr, struct ugdr_send_wr** bad_wr);

#endif /* _UGDR_API_H_ */
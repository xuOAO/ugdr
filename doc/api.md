control
struct
#define UGDR_DEVICE_NAME_MAX = 64
#define UGDR_MAX_GPUS_PER_DEV = 16
struct ugdr_device {
char name[UGDR_DEVICE_NAME_MAX];
int numa_node;
int num_gpu;
int gpus[UGDR_MAX_GPUS_PER_DEV];
}
enum ugdr_accsee_flags {
UGDR_ACCESS_LOCAL_WRITE = 1
};
struct ugdr_qp_init_attr {
void* qp_context;
ugdr_cq* send_cq;
ugdr_cq* recv_cq;
struct {
uint32_t max_send_wr;
uint32_t max_recv_wr;
uint32 max_sge // 暂时保留 max_sge = 1
}
int qp_type;//暂时保留 qp_type = rc
int sq_sig_all;
}
enum ugdr_qp_state {
UGDR_QPS_RESET,
UGDR_QPS_INIT,
UGDR_QPS_RTR,
UGDR_QPS_RTS,
UGDR_QPS_ERR
}
struct ugdr_qp_attr {
enum ugdr_qp_state qp_state;
enum ugdr_qp_state cur_qp_state;
uint32_t dest_qp_num;
}
struct ugdr_wc{
      uint64_t wr_id;
enum ugdr_wc_status;
enum ugdr_wc_opcode;
uint32_t byte_len;
uint32 qp_num;
}

/*
struct ugdr_mr{
struct ugdr_context* context;
struct ugdr_pd* pd;
void* addr;
size_t length;
uint32_t handle;
uint32_t lkey;
uint32_t rkey;
}
*/
func
struct ugdr_device** ugdr_get_device_list(int *num_devices)
struct ugdr_context* ugdr_open_device(struct ugdr_device* device)
struct ugdr_pd* ugdr_alloc_pd(struct ugdr_context* ctx)
struct ugdr_mr* ugdr_reg_mr(struct ugdr_pd* pd, void* addr, size_t length, enum ibv_access_flags access)
struct ugdr_cq* ugdr_create_cq(struct ugdr_context* ctx, int cqe)
struct ugdr_qp* ugdr_create_qp(struct ugdr_pd* pd, struct ugdr_qp_init_attr* init_attr)
int ugdr_modify_qp(struct ugdr_qp* qp, struct ugdr_qp_attr* attr, int attr_mask)
int ugdr_destroy_qp(struct ugdr_qp* qp)
int ugdr_destroy_cq(struct ugdr_cq* cq)
int ugdr_dereg_mr(struct ugdr_mr* mr)
int ugdr_dealloc_pd(struct ugdr_pd* pd)
int ugdr_close_device(struct ugdr_device* device)
int ugdr_poll_cq(struct ugdr_cq* cq, int num_entries, struct ugdr_wc *wc)
data
struct
enum ugdr_wr_opcode{
      UGDR_WR_SEND,
UGDR_WR_RDMA_WRITE,//暂不支持
UGDR_WR_RDMA_READ//赞不支持
}
enum ugdr_wc_status{
UGDR_WC_SUCCESS = 0,
UGDR_WC_LOC_LEN_ERR
}
struct ugdr_sge{
uint64_t addr;
uint32_t length;
uint32_t lkey;
}
enum ugdr_send_flags {
      UGDR_SEND_SIGNALED = 1,
UGDR_SEND_INLINE = (1 << 1)
}
struct ugdr_recv_wr{
uint64_t wr_id;
struct ugdr_recv_wr* next;
struct ugdr_sge* sg_list; //暂不支持多段sge
int num_sge; //num_sge = 1
}
struct ugdr_send_wr{
uint64_t wr_id;
struct ugdr_sge* sg_list;
int num_sge;
enum ugdr_wr_opcode opcode;
uint32_t send_flags;
struct{
uint64_t remote_addr;
uint32_t rkey;
}rdma;//暂不支持
}
func
int ugdr_post_recv(struct ugdr_qp* qp, struct ugdr_recv_wr* wr, struct ugdr_recv_wr** bad_wr)
int ugdr_post_send(struct ugdr_qp* qp, struct ugdr_send_wr* wr, struct ugdr_send_wr** bad_wr)

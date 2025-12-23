#ifndef UGDR_H_
#define UGDR_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C"{
#endif

struct ugdr_context;
struct ugdr_pd;
struct ugdr_cq;
struct ugdr_qp;

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

struct ugdr_cq* ugdr_create_cq(struct ugdr_context* ctx, int wqe);
int ugdr_destroy_cq(struct ugdr_cq* cq);

struct ugdr_qp* ugdr_create_qp(struct ugdr_pd* pd, struct ugdr_qp_init_attr* attr);
int ugdr_destroy_qp(struct ugdr_qp* qp);

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

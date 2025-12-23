#include "test_fixture.h"
#include "../include/ugdr.h"

// Test PD allocation and deallocation
TEST_F(DaemonTest, Resource_PD_AllocDealloc) {
    struct ugdr_context* ctx = ugdr_open_device("eth0");
    ASSERT_NE(ctx, nullptr);

    struct ugdr_pd* pd = ugdr_alloc_pd(ctx);
    ASSERT_NE(pd, nullptr);
    ASSERT_EQ(ugdr_dealloc_pd(ctx, pd), 0);

    ugdr_close_device(ctx);
}

// Test CQ creation and destruction
TEST_F(DaemonTest, Resource_CQ_CreateDestroy) {
    struct ugdr_context* ctx = ugdr_open_device("eth0");
    ASSERT_NE(ctx, nullptr);

    struct ugdr_cq* cq = ugdr_create_cq(ctx, 128);
    ASSERT_NE(cq, nullptr);
    ASSERT_EQ(ugdr_destroy_cq(cq), 0);

    ugdr_close_device(ctx);
}

// Test QP creation and destruction
TEST_F(DaemonTest, Resource_QP_CreateDestroy) {
    struct ugdr_context* ctx = ugdr_open_device("eth0");
    ASSERT_NE(ctx, nullptr);

    struct ugdr_pd* pd = ugdr_alloc_pd(ctx);
    ASSERT_NE(pd, nullptr);

    struct ugdr_cq* cq = ugdr_create_cq(ctx, 128);
    ASSERT_NE(cq, nullptr);

    struct ugdr_qp_init_attr qp_init_attr = {
        .qp_context = NULL,
        .send_cq = cq,
        .recv_cq = cq,
        .cap = {
            .max_send_wr = 128,
            .max_recv_wr = 128,
            .max_sge = 1,
        },
        .qp_type = 1, // RC
        .sq_sig_all = 1
    };

    struct ugdr_qp* qp = ugdr_create_qp(pd, &qp_init_attr);
    ASSERT_NE(qp, nullptr);

    ASSERT_EQ(ugdr_destroy_qp(qp), 0);
    ASSERT_EQ(ugdr_destroy_cq(cq), 0);
    ASSERT_EQ(ugdr_dealloc_pd(ctx, pd), 0);
    ugdr_close_device(ctx);
}

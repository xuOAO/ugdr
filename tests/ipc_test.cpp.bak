#include "test_fixture.h"
#include "../include/ugdr.h"

// Test for Completion Queue (CQ) experimental read/write
TEST_F(DaemonTest, IPC_CQ_ReadWrite) {
    struct ugdr_context* ctx = ugdr_open_device("eth0");
    ASSERT_NE(ctx, nullptr);

    struct ugdr_cq* cq = ugdr_create_cq(ctx, 128);
    ASSERT_NE(cq, nullptr);

    // Test multiple writes and reads
    for (int i = 0; i < 10; ++i) {
        int data_written = 42 + i;
        // Assuming 0 is success for experimental APIs
        ASSERT_EQ(ugdr_experimental_write_cq(cq, data_written), 0);
        int data_read = ugdr_experimental_read_cq(cq);
        ASSERT_EQ(data_written, data_read);
    }

    ASSERT_EQ(ugdr_destroy_cq(cq), 0);
    ugdr_close_device(ctx);
}

// Test for Queue Pair (QP) experimental read/write
TEST_F(DaemonTest, IPC_QP_ReadWrite) {
    struct ugdr_context* ctx = ugdr_open_device("eth0");
    ASSERT_NE(ctx, nullptr);

    struct ugdr_pd *pd = ugdr_alloc_pd(ctx);
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
        .qp_type = 1, // Assuming 1 is RC
        .sq_sig_all = 1
    };

    struct ugdr_qp *qp = ugdr_create_qp(pd, &qp_init_attr);
    ASSERT_NE(qp, nullptr);

    // Test SQ
    int data = 100;
    ASSERT_EQ(ugdr_experimental_write_qp_sq(qp, data), 0);
    int data_read_sq = ugdr_experimental_read_qp_sq(qp);
    ASSERT_EQ(data, data_read_sq);

    // Test RQ
    data++;
    ASSERT_EQ(ugdr_experimental_write_qp_rq(qp, data), 0);
    int data_read_rq = ugdr_experimental_read_qp_rq(qp);
    ASSERT_EQ(data, data_read_rq);

    ASSERT_EQ(ugdr_destroy_qp(qp), 0);
    ASSERT_EQ(ugdr_destroy_cq(cq), 0);
    ugdr_dealloc_pd(ctx, pd);
    ugdr_close_device(ctx);
}

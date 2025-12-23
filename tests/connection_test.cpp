#include "test_fixture.h"
#include "../include/ugdr.h"

constexpr int TEST_LOOPS = 100;

TEST_F(DaemonTest, ConnectionStability_OpenCloseDevice) {
    for (int i = 0; i < TEST_LOOPS; ++i) {
        struct ugdr_context* ctx = ugdr_open_device("eth0");
        ASSERT_NE(ctx, nullptr);
        struct ugdr_pd* pd = ugdr_alloc_pd(ctx);
        ASSERT_NE(pd, nullptr);
        usleep(10000); // sleep 10ms to simulate some work
        ugdr_dealloc_pd(ctx, pd);
        ugdr_close_device(ctx);
    }
}

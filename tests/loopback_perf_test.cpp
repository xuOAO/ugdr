#include "test_fixture.h"
#include "../include/ugdr.h"
#include <vector>
#include <chrono>

TEST_F(DaemonTest, Loopback_ShmRing_Throughput) {
    struct ugdr_context* ctx = ugdr_open_device("eth0");
    ASSERT_NE(ctx, nullptr);

    struct ugdr_pd* pd = ugdr_alloc_pd(ctx);
    ASSERT_NE(pd, nullptr);

    // Create a large CQ to minimize overflow risk during batching
    int cq_size = 4096;
    struct ugdr_cq* cq = ugdr_create_cq(ctx, cq_size);
    ASSERT_NE(cq, nullptr);

    struct ugdr_qp_init_attr qp_init_attr = {
        .qp_context = NULL,
        .send_cq = cq,
        .recv_cq = cq,
        .cap = {
            .max_send_wr = 4096,
            .max_recv_wr = 4096,
            .max_sge = 1,
        },
        .qp_type = 1, // RC
        .sq_sig_all = 0 // We will signal selectively or all, let's say 0 and set flag
    };
    struct ugdr_qp* qp = ugdr_create_qp(pd, &qp_init_attr);
    ASSERT_NE(qp, nullptr);

    // Register MR
    std::vector<char> buf(4096);
    struct ugdr_mr* mr = ugdr_reg_mr(pd, buf.data(), buf.size(), UGDR_ACCESS_LOCAL_WRITE);
    ASSERT_NE(mr, nullptr);

    // Prepare Send WR
    struct ugdr_sge sge;
    sge.addr = (uint64_t)buf.data(); 
    sge.length = 64;   
    sge.lkey = mr->lkey;      

    struct ugdr_send_wr wr;
    wr.wr_id = 1;
    wr.next = NULL;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = UGDR_WR_SEND;
    wr.send_flags = UGDR_SEND_SIGNALED; // Signal every WR for simplicity in counting
    
    struct ugdr_send_wr* bad_wr = NULL;

    // Warmup
    for(int i=0; i<100; ++i) {
        ASSERT_EQ(ugdr_post_send(qp, &wr, &bad_wr), 0);
        struct ugdr_wc wc;
        while(ugdr_poll_cq(cq, 1, &wc) == 0);
        ASSERT_EQ(wc.status, UGDR_WC_SUCCESS);
    }

    // Benchmark
    const int iterations = 10000; // Total 320k ops
    const int batch_size = 32;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for(int i=0; i<iterations; ++i) {
        // Post a batch
        for(int b=0; b<batch_size; ++b) {
             ASSERT_EQ(ugdr_post_send(qp, &wr, &bad_wr), 0);
        }
        
        // Poll completions
        int completions = 0;
        while(completions < batch_size) {
            struct ugdr_wc wc[batch_size];
            int n = ugdr_poll_cq(cq, batch_size - completions, wc);
            if (n > 0) {
                for(int k=0; k<n; ++k) {
                    if (wc[k].status != UGDR_WC_SUCCESS) {
                        FAIL() << "WC Status Error: " << wc[k].status;
                    }
                }
                completions += n;
            }
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;
    
    long long total_ops = (long long)iterations * batch_size;
    double iops = total_ops / diff.count();
    
    std::cout << "[          ] Total Ops: " << total_ops << std::endl;
    std::cout << "[          ] Time: " << diff.count() << " s" << std::endl;
    std::cout << "[          ] IOPS: " << iops << std::endl;
    std::cout << "[          ] Latency: " << (diff.count() * 1e6) / total_ops << " us" << std::endl;

    ugdr_dereg_mr(mr);
    ugdr_destroy_qp(qp);
    ugdr_destroy_cq(cq);
    ugdr_dealloc_pd(ctx, pd);
    ugdr_close_device(ctx);
}

TEST_F(DaemonTest, Loopback_ShmRing_Throughput_BatchPost) {
    struct ugdr_context* ctx = ugdr_open_device("eth0");
    ASSERT_NE(ctx, nullptr);

    struct ugdr_pd* pd = ugdr_alloc_pd(ctx);
    ASSERT_NE(pd, nullptr);

    // Create a large CQ to minimize overflow risk during batching
    int cq_size = 4096;
    struct ugdr_cq* cq = ugdr_create_cq(ctx, cq_size);
    ASSERT_NE(cq, nullptr);

    struct ugdr_qp_init_attr qp_init_attr = {
        .qp_context = NULL,
        .send_cq = cq,
        .recv_cq = cq,
        .cap = {
            .max_send_wr = 4096,
            .max_recv_wr = 4096,
            .max_sge = 1,
        },
        .qp_type = 1, // RC
        .sq_sig_all = 0 // We will signal selectively or all, let's say 0 and set flag
    };
    struct ugdr_qp* qp = ugdr_create_qp(pd, &qp_init_attr);
    ASSERT_NE(qp, nullptr);

    // Register MR
    std::vector<char> buf(4096);
    struct ugdr_mr* mr = ugdr_reg_mr(pd, buf.data(), buf.size(), UGDR_ACCESS_LOCAL_WRITE);
    ASSERT_NE(mr, nullptr);

    // Prepare Send WR
    struct ugdr_sge sge;
    sge.addr = (uint64_t)buf.data(); 
    sge.length = 64;   
    sge.lkey = mr->lkey;      

    const int batch_size = 32;
    std::vector<struct ugdr_send_wr> wrs(batch_size);
    for(int i=0; i<batch_size; ++i) {
        wrs[i].wr_id = i;
        wrs[i].next = (i == batch_size - 1) ? NULL : &wrs[i+1];
        wrs[i].sg_list = &sge;
        wrs[i].num_sge = 1;
        wrs[i].opcode = UGDR_WR_SEND;
        wrs[i].send_flags = UGDR_SEND_SIGNALED;
    }
    
    struct ugdr_send_wr* bad_wr = NULL;

    // Warmup
    for(int i=0; i<100; ++i) {
        ASSERT_EQ(ugdr_post_send(qp, &wrs[0], &bad_wr), 0);
        int completions = 0;
        while(completions < batch_size) {
            struct ugdr_wc wc[batch_size];
            int n = ugdr_poll_cq(cq, batch_size - completions, wc);
            if (n > 0) completions += n;
        }
    }

    // Benchmark
    const int iterations = 10000; // Total 320k ops
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for(int i=0; i<iterations; ++i) {
        // Post a batch
        ASSERT_EQ(ugdr_post_send(qp, &wrs[0], &bad_wr), 0);
        
        // Poll completions
        int completions = 0;
        while(completions < batch_size) {
            struct ugdr_wc wc[batch_size];
            int n = ugdr_poll_cq(cq, batch_size - completions, wc);
            if (n > 0) {
                for(int k=0; k<n; ++k) {
                    if (wc[k].status != UGDR_WC_SUCCESS) {
                        FAIL() << "WC Status Error: " << wc[k].status;
                    }
                }
                completions += n;
            }
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;
    
    long long total_ops = (long long)iterations * batch_size;
    double iops = total_ops / diff.count();
    
    std::cout << "[          ] Total Ops: " << total_ops << std::endl;
    std::cout << "[          ] Time: " << diff.count() << " s" << std::endl;
    std::cout << "[          ] IOPS: " << iops << std::endl;
    std::cout << "[          ] Latency: " << (diff.count() * 1e6) / total_ops << " us" << std::endl;

    ugdr_dereg_mr(mr);
    ugdr_destroy_qp(qp);
    ugdr_destroy_cq(cq);
    ugdr_dealloc_pd(ctx, pd);
    ugdr_close_device(ctx);
}

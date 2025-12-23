#include <cstdio>
#include <unistd.h>
#include "../include/ugdr.h"

int main() {
    struct ugdr_context* ctx = ugdr_open_device("eth0");
    if (!ctx) {
        printf("Failed to open device eth0\n");
        return -1;
    }
    struct ugdr_cq* cq = ugdr_create_cq(ctx, 128);
    if (!cq) {
        printf("Failed to create completion queue\n");
        ugdr_close_device(ctx);
        return -1;
    }

    int data = 42;
    ugdr_experimental_write_cq(cq, data);
    int data_read = ugdr_experimental_read_cq(cq);
    if (data == data_read) {
        printf("Successfully wrote and read data from cq: %d\n", data_read);
    } else {
        printf("Data mismatch: wrote %d but read %d in cq\n", data, data_read);
    }

    if ( ugdr_destroy_cq(cq) < 0) {
        printf("Failed to destroy completion queue\n");
        ugdr_close_device(ctx);
        return -1;
    }

    struct ugdr_pd *pd = ugdr_alloc_pd(ctx);
    struct ugdr_qp_init_attr qp_init_attr = {
        .qp_context = NULL,
        .send_cq = cq,
        .recv_cq = cq,
        .cap = {
            .max_send_wr = 1,
            .max_recv_wr = 1,
            .max_sge = 1,
        },
        .qp_type = 1,
        .sq_sig_all = 1
    };

    struct ugdr_qp *qp = ugdr_create_qp(pd, &qp_init_attr);
    if(qp == NULL) {
        printf("Failed to create queue pair\n");
        ugdr_close_device(ctx);
        return -1;
    }

    data++;
    ugdr_experimental_write_qp_sq(qp, data);
    data_read = ugdr_experimental_read_qp_sq(qp);
    if (data == data_read) {
        printf("Successfully wrote and read data from qp_sq: %d\n", data_read);
    } else {
        printf("Data mismatch: wrote %d but read %d in qp_sq\n", data, data_read);
    }

    data++;
    ugdr_experimental_write_qp_rq(qp, data);
    data_read = ugdr_experimental_read_qp_rq(qp);
    if (data == data_read) {
        printf("Successfully wrote and read data from qp_rq: %d\n", data_read);
    } else {
        printf("Data mismatch: wrote %d but read %d in qp_rq\n", data, data_read);
    }
    ugdr_destroy_qp(qp);

    ugdr_close_device(ctx);
    printf("Successfully created completion queue\n");
    return 0;
}
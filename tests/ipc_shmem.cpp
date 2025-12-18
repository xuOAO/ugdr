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
        printf("Successfully wrote and read data: %d\n", data_read);
    } else {
        printf("Data mismatch: wrote %d but read %d\n", data, data_read);
    }

    if ( ugdr_destroy_cq(cq) < 0) {
        printf("Failed to destroy completion queue\n");
        ugdr_close_device(ctx);
        return -1;
    }

    ugdr_close_device(ctx);
    printf("Successfully created completion queue\n");
    return 0;
}
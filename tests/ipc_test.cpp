#include <cstdio>
#include <unistd.h>
#include "../include/ugdr.h"

constexpr int TEST_LOOPS = 1000;

int main(){
    printf("Starting connection stability test...\n");
    for (int i = 0; i < TEST_LOOPS; ++i) {
        printf("\rLoop %d/%d...", i + 1, TEST_LOOPS);
        fflush(stdout);
        struct ugdr_context* ctx = ugdr_open_device("eth0");
        if (ctx){
            struct ugdr_pd* pd = ugdr_alloc_pd(ctx);
            usleep(10000); // sleep 10ms to simulate some work
            ugdr_dealloc_pd(ctx, pd);
            ugdr_close_device(ctx);
        } else{
            printf("\nFailed to open device eth0 on loop %d\n", i + 1);
            return -1;
        }
    }

    printf("\nSuccessfully completed %d open/close loops.\n", TEST_LOOPS);
    return 0;
}
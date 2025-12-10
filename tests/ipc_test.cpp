#include <cstdio>
#include "../include/ugdr.h"

int main(){
    struct ugdr_context* ctx = ugdr_open_device("eth0");
    if (ctx){
        printf("Successfully opened device eth0\n");
        ugdr_close_device(ctx);
    } else{
        printf("Failed to open device eth0\n");
    }
    return 0;
}
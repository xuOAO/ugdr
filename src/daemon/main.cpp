#include <cstdio>
#include "core/manager.h"

static ugdr::core::ManagerConfig config = {
    .driver_type = "loopback",
    .uds_path = ugdr::ipc::UDS_PATH_DEFAULT,
    .driver_config = {
        .eth_configs = {
            {.eth_name = "eth0"},
        },
    },
    .num_workers = 1,
};

int main(){
    ugdr::core::Manager manager(config);
    // manager.init();
    manager.run();
    return 0;
}
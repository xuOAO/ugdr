#include <cstdio>
#include "core/manager.h"

static ugdr::core::ManagerConfig config = {
    .uds_path = ugdr::ipc::UDS_PATH_DEFAULT,
    .eth_configs = {
        {
            .eth_name = "eth0",
        },
    },
};

int main(){
    ugdr::core::Manager manager(config);
    // manager.init();
    manager.run();
    return 0;
}
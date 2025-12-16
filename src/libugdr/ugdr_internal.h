#pragma once
#include "../common/ipc/socket_utils.h"
#include "../common/ugdr_types.h"

struct ugdr_context {
    ugdr::ipc::Socket sock;
    char dev_name[ugdr::common::UGDR_MAX_DEV_NAME_LEN];
};

struct ugdr_pd {
    ugdr::ipc::Socket* sock_ptr;
    uint32_t pd_handle;
};
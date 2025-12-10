#pragma once
#include "../common/ipc/socket_utils.h"
#include "../common/ugdr_types.h"

struct ugdr_context {
    ugdr::ipc::Socket sock;
    uint32_t ctx_idx;
    char dev_name[ugdr::common::UGDR_MAX_DEV_NAME_LEN];
};